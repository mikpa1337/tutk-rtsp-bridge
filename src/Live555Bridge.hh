#pragma once

// Everything that glues the decoded TUTK frame stream to live555:
// FrameQueue (thread-safe handoff from IOTCCameraClient's io_context thread
// to live555's event loop), TutkH264Source/TutkAudioSource (the FramedSource
// wrappers live555 pulls frames through), and the two
// OnDemandServerMediaSubsession implementations that wire those sources into
// an RTSP session. Used to be four separate files - merged since they're
// never used independently of each other.

#include <FramedSource.hh>
#include <OnDemandServerMediaSubsession.hh>
#include <H264VideoStreamFramer.hh>
#include <SimpleRTPSink.hh>
#include <liveMedia.hh>
#include <UsageEnvironment.hh>
#include <BasicUsageEnvironment.hh>
#include <Groupsock.hh>

#include "Logger.hh"

#include <vector>
#include <queue>
#include <mutex>
#include <cstdint>
#include <functional>
#include <chrono>

#if defined(_WIN32) || defined(_WIN64)
    #include <winsock2.h> // Provides 'struct timeval' on Windows
#else
    #include <sys/time.h>
#endif

// check this out 
#include <boost/circular_buffer.hpp>


// ---------------------------------------------------------------------
// FrameQueue
// ---------------------------------------------------------------------

struct TutkFrame
{
    std::vector<std::uint8_t> data;
    std::uint32_t timestampMs;
    bool isAudio;
};

// We no longer want NALs ever since i switched the streamer
struct FrameQueue
{
    static constexpr std::size_t kMaxQueuedFrames = 50;

    //std::queue<TutkFrame> frames;
    boost::circular_buffer<TutkFrame> frames{ kMaxQueuedFrames };
    std::mutex mtx;

    std::function<void()> onFrameAvailable;

    void push(std::vector<std::uint8_t> frame, std::uint32_t timestampMs, bool isAudio) {
        if (isAudio || frame.size() < 4) {
            pushInternal({ std::move(frame), timestampMs, isAudio });
            return;
        }

        // Helper to get start code length (3 or 4 bytes)
        auto getStartCodeLen = [](const std::uint8_t* ptr, std::size_t remaining) -> std::size_t {
            if (remaining >= 4 && ptr[0] == 0 && ptr[1] == 0 && ptr[2] == 0 && ptr[3] == 1) {
                return 4;
            }
            if (remaining >= 3 && ptr[0] == 0 && ptr[1] == 0 && ptr[2] == 1) {
                return 3;
            }
            return 0;
        };

        // Find all start code offsets and their lengths
        struct NalInfo {
            std::size_t offset;
            std::size_t codeLen;
        };
        std::vector<NalInfo> nals;

        for (std::size_t i = 0; i + 2 < frame.size(); ++i) {
            std::size_t codeLen = getStartCodeLen(&frame[i], frame.size() - i);
            if (codeLen > 0) {
                nals.push_back({ i, codeLen });
                i += codeLen - 1; // Skip past the start code bytes
            }
        }

        // Fallback: If no start code found at all, push as-is
        if (nals.empty()) {
            pushInternal({ std::move(frame), timestampMs, false });
            return;
        }

        // Slice NALs and STRIP start codes
        for (std::size_t k = 0; k < nals.size(); ++k) {
            std::size_t payloadStart = nals[k].offset + nals[k].codeLen; // <-- STRIPS START CODE
            std::size_t payloadEnd = (k + 1 < nals.size()) ? nals[k + 1].offset : frame.size();

            if (payloadStart < payloadEnd) {
                std::vector<std::uint8_t> nalPayload(frame.begin() + payloadStart, frame.begin() + payloadEnd);
                pushInternal({ std::move(nalPayload), timestampMs, false });
            }
        }
    }

    bool pop(TutkFrame& outFrame) {
        std::lock_guard<std::mutex> lock(mtx);
        if (frames.empty()) {
            return false;
        }
        outFrame = std::move(frames.front());
        frames.pop_front();
        return true;
    }

private:
    void pushInternal(TutkFrame&& tutkFrame) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(mtx);
            // boost::circular_buffer should auto drop/overwrite oldest elements
            frames.push_back(std::move(tutkFrame));

            callback = onFrameAvailable;
        }
        if (callback) { 
            callback();
        }
    }

    /*
    void pushInternal(TutkFrame&& tutkFrame) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(mtx);

            if (frames.size() >= kMaxQueuedFrames) {
                frames.pop();
            }

            //LOG_INFO("Queued NAL: size={}, start_bytes={:02x} {:02x} {:02x} {:02x}", tutkFrame.data.size(), tutkFrame.data[0], tutkFrame.data[1], tutkFrame.data[2], tutkFrame.data[3]);

            frames.push(std::move(tutkFrame));
            callback = onFrameAvailable;
        }

        if (callback) {
            callback();
        }
    }
    */
};


//
// Here lies the old FrameQueue implementation
// It did not push NALs correctly?
/*
struct FrameQueue
{
    // Ring-buffer cap: once full, push() drops the oldest frame rather than
    // growing unbounded. Live streaming - a backlog just means stale
    // frames, better to drop than to let this grow forever if a consumer
    // stalls.
    static constexpr std::size_t kMaxQueuedFrames = 50;

    std::queue<TutkFrame> frames;
    std::mutex mtx;

    std::function<void()> onFrameAvailable;

    void push(std::vector<std::uint8_t> frame, std::uint32_t timestampMs, bool isAudio) {
        {
            std::lock_guard<std::mutex> lock(mtx);

            if (frames.size() >= kMaxQueuedFrames) {
                frames.pop();
            }

            frames.push({ std::move(frame), timestampMs, isAudio });
        }
        if (onFrameAvailable) {
            onFrameAvailable();
        }
    }
};
*/

// ---------------------------------------------------------------------
// Timeval helpers
// ---------------------------------------------------------------------

// Confirmed fixed cadence, video: 25 fps exact. Trusted directly rather
// than derived per-frame (from arrival time, whatever) - deriving it just
// reintroduces the jitter this is meant to eliminate.
constexpr unsigned kVideoFrameIntervalMs = 40;
constexpr unsigned kVideoFrameIntervalUs = kVideoFrameIntervalMs * 1000;

// Audio is NOT the same cadence as video, despite an earlier assumption
// that it was - measured empirically (not from any spec): at the 8kHz mono
// config this project uses, real audio chunks are 800 bytes = 400 samples
// = 400/8000 = 50ms each, i.e. 20 chunks/sec. If the sample rate/channel
// count passed to TutkAudioServerMediaSubsession::createNew ever changes
// from 8000Hz/mono, this needs re-deriving against an actual chunk's byte
// size again - it is NOT computed from the sample rate here on purpose,
// because "should be 640 bytes" was already wrong once.
constexpr unsigned kAudioFrameIntervalMs = 50;
constexpr unsigned kAudioFrameIntervalUs = kAudioFrameIntervalMs * 1000;

inline timeval tutk_timestamp_to_timeval(std::uint32_t timestampMs) {
    timeval tv{};
    tv.tv_sec = static_cast<long>(timestampMs / 1000);
    tv.tv_usec = static_cast<long>((timestampMs % 1000) * 1000);
    return tv;
}

// ---------------------------------------------------------------------
// TutkH264Source / TutkAudioSource
// ---------------------------------------------------------------------

class TutkH264Source : public FramedSource
{
public:
    static TutkH264Source* createNew(UsageEnvironment& env, FrameQueue& queue) {
        return new TutkH264Source(env, queue);
    }

protected:
    TutkH264Source(UsageEnvironment& env, FrameQueue& queue)
        : FramedSource(env), m_queue(queue) {
        m_triggerId = env.taskScheduler().createEventTrigger(deliverFrame0);

        // Safely assign callback under queue lock
        std::lock_guard<std::mutex> lock(m_queue.mtx);
        m_queue.onFrameAvailable = [this]() {
            envir().taskScheduler().triggerEvent(m_triggerId, this);
            };
    }

    ~TutkH264Source() override {
        {
            std::lock_guard<std::mutex> lock(m_queue.mtx);
            m_queue.onFrameAvailable = nullptr;
        }

        if (m_triggerId != 0) {
            envir().taskScheduler().deleteEventTrigger(m_triggerId);
            m_triggerId = 0;
        }
    }

    void doGetNextFrame() override {
        deliverFrame();
    }

private:
    FrameQueue& m_queue;
    EventTriggerId m_triggerId{};

    static void deliverFrame0(void* clientData) {
        static_cast<TutkH264Source*>(clientData)->deliverFrame();
    }

    void deliverFrame() {
        if (!isCurrentlyAwaitingData()) {
            return;
        }

        TutkFrame frameData;
        // Use the thread-safe circular_buffer pop helper
        if (!m_queue.pop(frameData)) {
            return;
        }

        fFrameSize = (std::min)(frameData.data.size(), static_cast<std::size_t>(fMaxSize));
        std::memcpy(fTo, frameData.data.data(), fFrameSize);

        fNumTruncatedBytes = frameData.data.size() > fMaxSize
            ? static_cast<unsigned>(frameData.data.size() - fMaxSize)
            : 0;

        if (fNumTruncatedBytes > 0) {
            LOG_WARN("H264 frame truncated by {} bytes - OutPacketBuffer::maxSize is too small ({})",
                fNumTruncatedBytes, OutPacketBuffer::maxSize);
        }

        fPresentationTime = tutk_timestamp_to_timeval(frameData.timestampMs);
        fDurationInMicroseconds = kVideoFrameIntervalUs;

        FramedSource::afterGetting(this);
    }
};

class TutkAudioSource : public FramedSource
{
public:
    static TutkAudioSource* createNew(UsageEnvironment& env, FrameQueue& queue, unsigned sampleRate, unsigned numChannels) {
        return new TutkAudioSource(env, queue, sampleRate, numChannels);
    }

protected:
    TutkAudioSource(UsageEnvironment& env, FrameQueue& queue, unsigned sampleRate, unsigned numChannels)
        : FramedSource(env), m_queue(queue), m_sampleRate(sampleRate), m_numChannels(numChannels) {
        m_triggerId = env.taskScheduler().createEventTrigger(deliverFrame0);

        {
            std::lock_guard<std::mutex> lock(m_queue.mtx);
            m_queue.onFrameAvailable = [this]() {
                envir().taskScheduler().triggerEvent(m_triggerId, this);
                };
        }

        m_expectedBytesPerChunk = static_cast<std::size_t>(m_numChannels) * kBytesPerSample
            * m_sampleRate * kAudioFrameIntervalMs / 1000;
    }

    ~TutkAudioSource() override {
        {
            std::lock_guard<std::mutex> lock(m_queue.mtx);
            m_queue.onFrameAvailable = nullptr;
        }

        if (m_triggerId != 0) {
            envir().taskScheduler().deleteEventTrigger(m_triggerId);
            m_triggerId = 0;
        }
    }

    void doGetNextFrame() override {
        deliverFrame();
    }

private:
    FrameQueue& m_queue;
    EventTriggerId m_triggerId{};
    unsigned m_sampleRate;
    unsigned m_numChannels;
    std::size_t m_expectedBytesPerChunk{};
    std::size_t m_lastMismatchedSize{};

    static constexpr unsigned kBytesPerSample = 2; // 16-bit PCM

    static void deliverFrame0(void* clientData) {
        static_cast<TutkAudioSource*>(clientData)->deliverFrame();
    }

    void deliverFrame() {
        if (!isCurrentlyAwaitingData()) return;

        TutkFrame frame;
        if (!m_queue.pop(frame)) return;

        fFrameSize = (std::min)(frame.data.size(), static_cast<std::size_t>(fMaxSize));
        std::memcpy(fTo, frame.data.data(), fFrameSize);

        fNumTruncatedBytes = frame.data.size() > fMaxSize
            ? static_cast<unsigned>(frame.data.size() - fMaxSize)
            : 0;

        fPresentationTime = tutk_timestamp_to_timeval(frame.timestampMs);
        fDurationInMicroseconds = kAudioFrameIntervalUs;

        if (m_expectedBytesPerChunk != 0 && frame.data.size() != m_expectedBytesPerChunk
            && frame.data.size() != m_lastMismatchedSize) {
            m_lastMismatchedSize = frame.data.size();
            LOG_WARN("audio chunk is {} bytes, expected {} for {} Hz/{} ch at the assumed {} ms cadence - audio timing may be wrong",
                frame.data.size(), m_expectedBytesPerChunk, m_sampleRate, m_numChannels, kAudioFrameIntervalMs);
        }

        FramedSource::afterGetting(this);
    }
};

/*
class TutkH264Source : public FramedSource
{
public:
    static TutkH264Source* createNew(UsageEnvironment& env, FrameQueue& queue) {
        return new TutkH264Source(env, queue);
    }
protected:
    TutkH264Source(UsageEnvironment& env, FrameQueue& queue)
        : FramedSource(env), m_queue(queue) {
        m_triggerId = env.taskScheduler().createEventTrigger(deliverFrame0);
        m_queue.onFrameAvailable = [this]() {
            envir().taskScheduler().triggerEvent(m_triggerId, this);
        };
    }

    ~TutkH264Source() override {
        m_queue.onFrameAvailable = {};
        if (m_triggerId != 0) {
            envir().taskScheduler().deleteEventTrigger(m_triggerId);
            m_triggerId = 0;
        }
    }

    void doGetNextFrame() override {
        deliverFrame();
    }

private:
    FrameQueue& m_queue;
    EventTriggerId m_triggerId{};

    static void deliverFrame0(void* clientData) {
        static_cast<TutkH264Source*>(clientData)->deliverFrame();
    }

    void deliverFrame() {
        if (!isCurrentlyAwaitingData()) {
            return;
        }

        TutkFrame frameData;
        {
            std::lock_guard<std::mutex> lock(m_queue.mtx);
            if (m_queue.frames.empty()) {
                return;
            }

            frameData = std::move(m_queue.frames.front());
            m_queue.frames.pop();
        }

        fFrameSize = (std::min)(frameData.data.size(), (std::size_t)fMaxSize);
        std::memcpy(fTo, frameData.data.data(), fFrameSize);

        // MUST tell live555 if we couldn't fit the whole frame - this used
        // to be commented out, which left fNumTruncatedBytes holding
        // whatever value happened to be there from a previous frame (or
        // uninitialized, on the very first one).
        fNumTruncatedBytes = frameData.data.size() > fMaxSize
            ? static_cast<unsigned>(frameData.data.size() - fMaxSize)
            : 0;

        if (fNumTruncatedBytes > 0) {
            LOG_WARN("H264 frame truncated by {} bytes - OutPacketBuffer::maxSize is too small ({})", fNumTruncatedBytes, OutPacketBuffer::maxSize);
        }
        //LOG_INFO("Delivering frame: actual size = {}, downstream fMaxSize = {}", frameData.data.size(), fMaxSize);

        // m_queue only ever holds video frames (it's IOTCCameraClient's
        // videoQueue) - frameData.isAudio should always be false here.
        fPresentationTime = tutk_timestamp_to_timeval(frameData.timestampMs);

        fDurationInMicroseconds = kVideoFrameIntervalUs; // 25 fps, confirmed exact
        FramedSource::afterGetting(this);
    }
};

class TutkAudioSource : public FramedSource
{
public:
    static TutkAudioSource* createNew(UsageEnvironment& env, FrameQueue& queue, unsigned sampleRate, unsigned numChannels) {
        return new TutkAudioSource(env, queue, sampleRate, numChannels);
    }

protected:
    TutkAudioSource(UsageEnvironment& env, FrameQueue& queue, unsigned sampleRate, unsigned numChannels)
        : FramedSource(env), m_queue(queue), m_sampleRate(sampleRate), m_numChannels(numChannels) {
        m_triggerId = env.taskScheduler().createEventTrigger(deliverFrame0);
        m_queue.onFrameAvailable = [this]() {
            envir().taskScheduler().triggerEvent(m_triggerId, this);
        };

        // Computed once here rather than every deliverFrame() call - this
        // is a sanity check, not a timing source (see deliverFrame), so it
        // doesn't need to be recomputed per-frame, and re-running the same
        // arithmetic ~20 times/sec on the live555 event-loop thread for no
        // reason was wasted work sharing that thread with video delivery.
        m_expectedBytesPerChunk = static_cast<std::size_t>(m_numChannels) * kBytesPerSample
            * m_sampleRate * kAudioFrameIntervalMs / 1000;
    }

    ~TutkAudioSource() override {
        m_queue.onFrameAvailable = {};
        // NOTE: this was `if (m_triggerId == 0)` - inverted, so it deleted
        // the trigger only when there was nothing to delete (a no-op call
        // with id 0) and skipped it whenever there actually was one,
        // leaking an event trigger every time an audio subsession tore
        // down. Matches TutkH264Source's destructor now.
        if (m_triggerId != 0) {
            envir().taskScheduler().deleteEventTrigger(m_triggerId);
            m_triggerId = 0;
        }
    }

    void doGetNextFrame() override {
        deliverFrame();
    }

private:
    FrameQueue& m_queue;
    EventTriggerId m_triggerId{};
    unsigned m_sampleRate;
    unsigned m_numChannels;
    std::size_t m_expectedBytesPerChunk{};
    std::size_t m_lastMismatchedSize{}; // 0 = "haven't logged a mismatch yet"

    // 16-bit PCM = 2 bytes per channel
    static constexpr unsigned kBytesPerSample = 2;

    static void deliverFrame0(void* clientData) {
        static_cast<TutkAudioSource*>(clientData)->deliverFrame();
    }

    void deliverFrame() {
        if (!isCurrentlyAwaitingData()) return;

        TutkFrame frame;
        {
            std::lock_guard<std::mutex> lock(m_queue.mtx);
            if (m_queue.frames.empty()) return;

            frame = std::move(m_queue.frames.front());
            m_queue.frames.pop();
        }

        fFrameSize = (std::min)(frame.data.size(), (std::size_t)fMaxSize);
        std::memcpy(fTo, frame.data.data(), fFrameSize);
        fNumTruncatedBytes = frame.data.size() > fMaxSize
            ? static_cast<unsigned>(frame.data.size() - fMaxSize)
            : 0;

        // frame.timestampMs here is NOT the camera's raw per-packet
        // timestamp - audio gets no reassembly/sync at all on the wire (see
        // AVAPI.hh's DualStreamProcessor), so that value can't be trusted
        // directly the way video's can. IOTCCameraClient's MediaClock
        // overwrites it with a locally-maintained clock, anchored to the
        // first video frame and advanced by frmNo distance * the confirmed
        // audio chunk interval, before this ever reaches the queue - reusing
        // the same conversion video uses keeps both streams on one
        // consistent, jitter-free timeline instead of two independently-paced
        // ones.
        fPresentationTime = tutk_timestamp_to_timeval(frame.timestampMs);
        fDurationInMicroseconds = kAudioFrameIntervalUs;

        // Sanity check, not a timing source: flags if a chunk's actual byte
        // count stops matching what the confirmed cadence implies (codec
        // change, camera firmware update, wrong assumption to begin with -
        // kAudioFrameIntervalMs's own comment is a live example of that last
        // one). Doesn't affect playback either way - duration above is fixed
        // regardless. Logs only when the mismatched size actually changes,
        // not on every single frame - a wrong assumption is exactly as wrong
        // whether it's logged once or 20 times a second, and the latter is
        // just noise competing for time on the same thread that delivers
        // video frames.
        if (m_expectedBytesPerChunk != 0 && frame.data.size() != m_expectedBytesPerChunk
            && frame.data.size() != m_lastMismatchedSize) {
            m_lastMismatchedSize = frame.data.size();
            LOG_WARN("audio chunk is {} bytes, expected {} for {} Hz/{} ch at the assumed {} ms cadence - audio timing may be wrong",
                      frame.data.size(), m_expectedBytesPerChunk, m_sampleRate, m_numChannels, kAudioFrameIntervalMs);
        }

        FramedSource::afterGetting(this);
    }
};
*/
// ---------------------------------------------------------------------
// Subsessions
// ---------------------------------------------------------------------

class TutkServerMediaSubsession : public OnDemandServerMediaSubsession
{
public:
    static TutkServerMediaSubsession* createNew(UsageEnvironment& env, FrameQueue& queue) {
        return new TutkServerMediaSubsession(env, queue);
    }

protected:
    TutkServerMediaSubsession(UsageEnvironment& env, FrameQueue& queue)
        : OnDemandServerMediaSubsession(env, True), m_queue(queue) {
    }

    FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate) override {
        estBitrate = 5000; // kbps - rough guess, live555 uses this for SDP only, doesn't enforce it
        OutPacketBuffer::maxSize = 2 * 1024 * 1024; // 

        auto* baseSource = TutkH264Source::createNew(envir(), m_queue);
        //return H264VideoStreamFramer::createNew(envir(), baseSource);

        // This actually wants the NALs stripped.
        return H264VideoStreamDiscreteFramer::createNew(envir(), baseSource);
    }

    RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource* inputSource) override {
        return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
    }

private:
    FrameQueue& m_queue;
};

class TutkAudioServerMediaSubsession : public OnDemandServerMediaSubsession {
public:
    static TutkAudioServerMediaSubsession* createNew(UsageEnvironment& env, FrameQueue& audioQueue,
        unsigned samplingFrequency = 8000,
        unsigned numChannels = 1) {
        return new TutkAudioServerMediaSubsession(env, audioQueue, samplingFrequency, numChannels);
    }

protected:
    TutkAudioServerMediaSubsession(UsageEnvironment& env, FrameQueue& audioQueue,
        unsigned samplingFrequency, unsigned numChannels)
        : OnDemandServerMediaSubsession(env, True),
        m_audioQueue(audioQueue),
        m_sampleRate(samplingFrequency),
        m_numChannels(numChannels) {
    }

    FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate) override {
        // kbps, for SDP only. 16-bit uncompressed PCM: sampleRate * 16 bits * channels / 1000.
        estBitrate = (m_sampleRate * 16 * m_numChannels) / 1000;
        return TutkAudioSource::createNew(envir(), m_audioQueue, m_sampleRate, m_numChannels);
    }

    RTPSink* createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
        FramedSource* inputSource) override {
        // Dynamic payload type 96, "audio/L16" for 16-bit uncompressed PCM
        return SimpleRTPSink::createNew(
            envir(), rtpGroupsock, 96, m_sampleRate,
            "audio", "L16", m_numChannels
        );
    }

private:
    FrameQueue& m_audioQueue;
    unsigned m_sampleRate;
    unsigned m_numChannels;
};