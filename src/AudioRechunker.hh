#pragma once

// Re-slices audio PCM from its native wire chunking (currently 800 bytes /
// 50ms per AVAPI.hh packet, confirmed empirically - see Live555Bridge.hh's
// kAudioFrameIntervalMs) into fixed-size pieces aligned to video's cadence
// (kVideoFrameIntervalMs), so every audio slice handed to live555 is the
// same duration as a video frame instead of two independently-paced grids.
//
// NOT required for audio/video to play back in sync - RTP timestamps each
// stream independently, a receiver doesn't need matching packet boundaries
// between them. This exists to simplify the downstream story (uniform
// slice duration) rather than to fix a correctness bug.
//
// Deliberately standalone (only needs Boost.CircularBuffer + Logger.hh, not
// live555) so it's testable in isolation - the discontinuity handling below
// is exactly the kind of "looks right, isn't" arithmetic worth a real test
// rather than eyeballing.

#include <boost/circular_buffer.hpp>

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

#include "Logger.hh"

class AudioRechunker
{
public:
    // sliceIntervalMs: duration each output slice should represent (pass
    // Live555Bridge.hh's kVideoFrameIntervalMs to align with video).
    // sampleRate/numChannels: same values passed to
    // TutkAudioServerMediaSubsession::createNew - used only to convert
    // between bytes and milliseconds, not assumed/hardcoded (kAudioFrameIntervalMs
    // was a hardcoded, wrong assumption once already - this isn't repeating
    // that mistake for the slice size).
    AudioRechunker(unsigned sampleRate, unsigned numChannels, unsigned sliceIntervalMs)
        : m_bytesPerMs(static_cast<std::size_t>(sampleRate) * numChannels * kBytesPerSample / 1000)
        , m_sliceIntervalMs(sliceIntervalMs)
        , m_sliceBytes(m_bytesPerMs * sliceIntervalMs)
        , m_buffer(m_sliceBytes * 8) // headroom: several slices worth, never expected to fill given draining happens every feed()
    {
    }

    std::size_t sliceBytes() const { return m_sliceBytes; }

    // `chunkStartTimestampMs` is the already-correctly-computed (gap-aware,
    // via IOTCCameraClient's MediaClock) real-world timestamp for the FIRST
    // byte of `data`. Assumes bytes within one incoming chunk are evenly
    // paced, which holds for raw fixed-rate PCM.
    //
    // onSlice is called once per completed slice as
    // (std::span<const std::uint8_t> sliceBytes, std::uint32_t sliceTimestampMs).
    template <class OnSlice>
    void feed(std::span<const std::uint8_t> data, std::uint32_t chunkStartTimestampMs, OnSlice&& onSlice)
    {
        if (!m_buffer.empty())
        {
            // What timestamp would this new chunk need to start at for the
            // buffered leftover to be genuinely contiguous with it?
            std::uint32_t expected = m_bufferStartTimestampMs
                + static_cast<std::uint32_t>(m_buffer.size() / m_bytesPerMs);

            if (chunkStartTimestampMs != expected)
            {
                // Discontinuity - almost certainly a wire packet AudioSequencer
                // (AVAPI.hh) already flagged as lost. The leftover bytes
                // can't be meaningfully glued to non-contiguous new data
                // without corrupting both the audio and every timestamp
                // after it, so drop them and resync on this chunk instead.
                LOG_WARN("audio rechunker: discontinuity ({} buffered bytes expected to lead into {} ms, next chunk starts at {} ms) - dropping leftover and resyncing",
                          m_buffer.size(), expected, chunkStartTimestampMs);
                m_buffer.clear();
            }
        }

        if (m_buffer.empty())
        {
            m_bufferStartTimestampMs = chunkStartTimestampMs;
        }

        m_buffer.insert(m_buffer.end(), data.begin(), data.end());

        while (m_buffer.size() >= m_sliceBytes)
        {
            std::vector<std::uint8_t> slice(m_buffer.begin(), m_buffer.begin() + m_sliceBytes);
            m_buffer.erase_begin(m_sliceBytes);

            onSlice(std::span<const std::uint8_t>(slice), m_bufferStartTimestampMs);

            m_bufferStartTimestampMs += m_sliceIntervalMs;
        }
    }

private:
    static constexpr unsigned kBytesPerSample = 2; // 16-bit PCM

    std::size_t m_bytesPerMs;
    unsigned m_sliceIntervalMs;
    std::size_t m_sliceBytes;

    boost::circular_buffer<std::uint8_t> m_buffer;
    std::uint32_t m_bufferStartTimestampMs{};
};
