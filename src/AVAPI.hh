#pragma once

#include <cstdint>
#include <vector>
#include <cstring>      // original product uses strcpy...
#include <bit>          // std::byteswap
#include <span>         // std::span
#include <functional>   // std::function
#include <map>
#include <bitset>
#include <list>
#include <mutex>
#include <optional>
#include <algorithm>
#include <ranges>

#include "Logger.hh"

// -----------------------------------------------------------------------
// BlockFifo: reassembles the raw UDP-sized chunks a frame arrives in back
// into whole frames, tracking loss per frame. Originally its own
// block_fifo.hpp - folded in here since AVAPI.hh (specifically
// FrameAssembler below) is its only consumer.
// -----------------------------------------------------------------------

// Replaces _block and handles automatic memory management
struct Block {
    uint32_t frmNo = 0;
    uint32_t frmSize = 0;
    uint16_t pos = 0;
    uint16_t frmInfoSize = 0;
    std::vector<uint8_t> buffer; // Automatically replaces i_buffer and p_buffer
};

// Replaces _block_fifo
class BlockFifo {
private:
    mutable std::mutex mutex_;
    std::list<Block> blocks_;
    size_t total_size_bytes_ = 0;

public:
    BlockFifo() = default;
    ~BlockFifo() = default;

    // Mutexes are non-copyable, so we explicitly delete copy/move semantics
    BlockFifo(const BlockFifo&) = delete;
    BlockFifo& operator=(const BlockFifo&) = delete;

    void clear() {
        std::scoped_lock lock(mutex_);
        blocks_.clear();
        total_size_bytes_ = 0;
    }

    size_t push(Block block) {
        std::scoped_lock lock(mutex_);
        total_size_bytes_ += block.buffer.size();
        blocks_.push_back(std::move(block));
        return total_size_bytes_;
    }

    size_t push_bypass_same_block(Block block) {
        std::scoped_lock lock(mutex_);

        auto it = std::ranges::find_if(blocks_, [&](const Block& b) {
            return b.frmNo == block.frmNo && b.pos == block.pos;
            });

        if (it != blocks_.end()) {
            return 0; // Block already exists, bypass
        }

        total_size_bytes_ += block.buffer.size();
        blocks_.push_back(std::move(block));
        return total_size_bytes_;
    }

    std::optional<Block> pop() {
        std::scoped_lock lock(mutex_);
        if (blocks_.empty()) {
            return std::nullopt;
        }

        Block b = std::move(blocks_.front());
        blocks_.pop_front();
        total_size_bytes_ -= b.buffer.size();
        return b;
    }

    std::optional<Block> pop_by_frm_no(uint32_t frmNo) {
        std::scoped_lock lock(mutex_);
        auto it = std::ranges::find_if(blocks_, [frmNo](const Block& b) {
            return b.frmNo == frmNo;
            });

        if (it != blocks_.end()) {
            Block b = std::move(*it);
            total_size_bytes_ -= b.buffer.size();
            blocks_.erase(it);
            return b;
        }
        return std::nullopt;
    }

    void remove_frame_by_frm_no(uint32_t frmNo) {
        std::scoped_lock lock(mutex_);
        blocks_.remove_if([&](const Block& b) {
            if (b.frmNo == frmNo) {
                total_size_bytes_ -= b.buffer.size();
                return true;
            }
            return false;
            });
    }

    std::optional<Block> seek_by_frm_no_pos(uint32_t frmNo, uint16_t pos) const {
        std::scoped_lock lock(mutex_);
        auto it = std::ranges::find_if(blocks_, [&](const Block& b) {
            return b.frmNo == frmNo && b.pos == pos;
            });

        if (it != blocks_.end()) {
            return *it; // Return a copy to preserve thread safety
        }
        return std::nullopt;
    }

    struct LostPositions {
        bool lostFrame = true;
        std::vector<uint16_t> positions;
    };

    LostPositions get_lost_pos(uint32_t frmNo) const {
        std::scoped_lock lock(mutex_); // Fixes missing lock in original code
        LostPositions result;

        std::vector<bool> found_pos(2048, false); // Replaces arbitrary 2048 stack array
        int expected_blocks = 0;

        for (const auto& b : blocks_) {
            if (b.frmNo == frmNo) {
                result.lostFrame = false;

                // Bounds safety check absent in original C code
                if (b.pos < found_pos.size()) {
                    found_pos[b.pos] = true;
                }

                if (expected_blocks == 0) {
                    uint32_t total_size = b.frmSize + b.frmInfoSize;
                    expected_blocks = total_size / 1024;
                    if ((total_size % 1024) != 0) { // Equivalent to v6 & 0x3FF
                        expected_blocks++;
                    }
                }
            }
        }

        if (!result.lostFrame && expected_blocks > 0) {
            for (int i = 0; i < expected_blocks && i < 2048; ++i) {
                if (!found_pos[i]) {
                    result.positions.push_back(static_cast<uint16_t>(i));
                }
            }
        }

        return result;
    }

    size_t size_bytes() const {
        std::scoped_lock lock(mutex_);
        return total_size_bytes_;
    }

    size_t count() const {
        std::scoped_lock lock(mutex_);
        return blocks_.size();
    }

    size_t count_by_frm_no(uint32_t frmNo) const {
        std::scoped_lock lock(mutex_);
        return std::ranges::count_if(blocks_, [frmNo](const Block& b) {
            return b.frmNo == frmNo;
            });
    }

    struct FrameInfo {
        uint32_t size = 0;
        uint32_t infoSize = 0;
    };

    std::optional<FrameInfo> get_info_by_frm_no(uint32_t frmNo) const {
        std::scoped_lock lock(mutex_);
        auto it = std::ranges::find_if(blocks_, [frmNo](const Block& b) {
            return b.frmNo == frmNo;
            });

        if (it != blocks_.end()) {
            return FrameInfo{ it->frmSize, it->frmInfoSize };
        }
        return std::nullopt;
    }

    void remove_all_frames_less_than(uint32_t frmNo) {
        std::scoped_lock lock(mutex_);
        blocks_.remove_if([&](const Block& b) {
            if (b.frmNo < frmNo) {
                total_size_bytes_ -= b.buffer.size();
                return true;
            }
            return false;
            });
    }
};


namespace AVAPI
{
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;

    //
    // types
    //

    enum class AVPacketType : u8
    {
        CONTROL = 0x00, // control messages?, set in AVInfo_Resend, _doClientAVTrans, avClientStart_inner
        STREAM  = 0x01, // media stream?, set in avSendFrameData+16C and +39E, __Audio_Send_Data+78
    };

    enum class AVOpCode : u8
    {
        // login
        AV_LOGIN            = 0x0,
        AV_LOGIN_RESP       = 0x1,
        
        // AV data
        VIDEO_DATA          = 0x3,
        AUDIO_DATA          = 0x4,
        AUDIO_DATA_RESEND   = 0x5,
        VIDEO_DATA_RESEND   = 0x6,
        VIDEO_DATA_REQUEST  = 0x7,
        VIDEO_DATA_OK       = 0x8,
        VIDEO_DATA_OK_ACK   = 0x9,
        VIDEO_DATA_LOST     = 0xA,
        
        // no clue
        IOCTRL_INNER        = 0x10, // Use this opcode, when sending system IOCTRL
        IOCTRL_INNER_ACK    = 0x11, // ACK
        
        // user IOCTRL
        IOCTRL_USER         = 0x70, // Use this opcode, when sending USER IOCTRL (!!!)
        IOCTRL_USER_ACK     = 0x71  // ACK
    };

    enum class CodecID : u16 {
        UNKNOWN         = 0x00,

        VIDEO_MPEG4     = 0x4C,
        VIDEO_H263      = 0x4D,
        VIDEO_H264      = 0x4E,
        VIDEO_MJPEG     = 0x4F,
        VIDEO_HEVC      = 0x50, // was added in 2014, hard to find the header

        AUDIO_AAC       = 0x88,
        AUDIO_G711U     = 0x89,
        AUDIO_G711A     = 0x8A,
        AUDIO_ADPCM     = 0x8B,
        AUDIO_PCM       = 0x8C,
        AUDIO_SPEEX     = 0x8D,
        AUDIO_MP3       = 0x8E,
        AUDIO_G726      = 0x8F,
    };

    enum class FrameFlag : u8 {
        PBFRAME = 0x00, // P/B frame
        IFRAME  = 0x01, // A/V i-frame
        MD      = 0x02, // motion detection
        IO      = 0x03, // alarm IO
    };

    enum class AudioSampleRate : u8 {
        SAMPLE_8K  = 0x00,
        SAMPLE_11K = 0x01,
        SAMPLE_12K = 0x02,
        SAMPLE_16K = 0x03,
        SAMPLE_22K = 0x04,
        SAMPLE_24K = 0x05,
        SAMPLE_32K = 0x06,
        SAMPLE_44K = 0x07,
        SAMPLE_48K = 0x08,
    };

    // Turns out windows has macros for DATABITS_8, etc, so i cant use it LOL
    enum class AudioDataBits : std::uint8_t {
        _8 = 0x00,
        _16 = 0x01,
    };

    enum class AudioChannel : std::uint8_t {
        MONO = 0,
        STEREO = 1,
    };

    //
    // --- structs
    //

#pragma pack(push, 1)
    struct AVPacketHead
    {
        AVPacketType type{};            // protocol type?
        AVOpCode opCode{};              // AVOpCode
        std::uint16_t ver{ 1 };         // version, always 1
        std::uint32_t frmNo{};          // frame sequence number
        std::uint32_t frmSize{};        // expected video data size
        std::uint16_t packNoInFrm{};    // chunk index
        std::uint16_t frmInfoSize{};    // FRAMEINFO size
        std::uint16_t payload{};        // size of body (sometimes ioCtrlBufSize+4)
        std::uint8_t reserve1[6]{};     // pad/align
    };
#pragma pack(pop)

#pragma pack(push, 1)
    struct FRAMEINFO
    {
        CodecID codecId;    // AVFRAMEINFO.h: 'defined in sys_mmdef.h'. PCMLE16 for audio, VIDEO_H264 for video
        u8 flags;         // combined with IPC_FRAME_xxx (frame flags)
        u8 camIndex;      // 0 - n
        u8 onlineNum;     // number of clients connected to the device
        u8 reserve1[3];   // ??? must be alignment
        u32 reserve2;     // ??? must be alignment
        u32 timestamp;    // timestamp of the frame, in milliseconds

        // these were commented out
        // std::uint32_t videoWidth
        // std::uint32_t videoHeight;
    };
#pragma pack(pop)

    // Apparently this is true
    // Audio Frame: flags = (samplerate << 2) | (databits << 1) | (channel)
    struct Frame {
        u32 frameNo;
        std::vector<u8> video;
        FRAMEINFO frameInfo;
        //bool complete{};
    };

    struct AVPacket
    {
        AVPacketHead protoHead;
        std::vector<std::uint8_t> body;

        [[nodiscard]] std::vector<u8> serialize() const {
            std::vector<u8> data(sizeof(AVPacketHead) + body.size(), 0);
            std::memcpy(data.data(), &protoHead, sizeof(AVPacketHead));
            if (!body.empty()) {
                std::memcpy(data.data() + sizeof(AVPacketHead), body.data(), body.size());
            }
            return data;
        }

        [[nodiscard]] static std::optional<AVPacket> deserialize(std::span<const u8> data) {
            if (data.size() < sizeof(AVPacketHead)) {
                return std::nullopt;
            }

            AVPacket packet{};
            std::memcpy(&packet.protoHead, data.data(), sizeof(AVPacketHead));

            auto bodySize = data.size() - sizeof(AVPacketHead);
            packet.body.resize(bodySize); // was reserve() - left body.size() == 0 and the memcpy below writing past the (empty) vector's logical size
            std::memcpy(packet.body.data(), data.data() + sizeof(AVPacketHead), bodySize);

            return packet;
        }
    };

    // IOCTRL sub-command codes used inside IOCTRL_USER packets (SMsgAVIoctrlAVStream body).
    namespace IoctrlType {
        constexpr u16 IPCAM_START       = 0x01FF;   // start video stream
        constexpr u16 IPCAM_STOP        = 0x02FF;   // stop video stream
        constexpr u16 IPCAM_AUDIO_START = 0x0300;   // start audio stream
        constexpr u16 IPCAM_AUDIO_STOP  = 0x0301;   // stop audio stream
    }

    class AVPacketFactory
    {
    public:
        static AVPacket createLogin(std::string_view username, std::string_view password) {
            AVPacket pkt{};
            pkt.protoHead.type      = AVPacketType::CONTROL;
            pkt.protoHead.opCode    = AVOpCode::AV_LOGIN;
            pkt.protoHead.ver       = 1;
            pkt.protoHead.payload   = 32;

            std::array<u8, 32> loginPack{};
            std::memcpy(loginPack.data(), username.data(), std::min(username.size(), std::size_t(15)));
            std::memcpy(loginPack.data() + 16, password.data(), std::min(password.size(), std::size_t(15)));

            pkt.body.assign(loginPack.begin(), loginPack.end());
            return pkt;
        }

        // tokenBytes is the 4-byte trailer echoed back in the AV_LOGIN_RESP - not
        // confirmed whether the camera actually validates it, but it's cheap to send.
        static AVPacket createVideoStart(const u8* tokenBytes) {
            return createIoctrlStart(IoctrlType::IPCAM_START, /*frmNo=*/1, tokenBytes);
        }

        // Same wire struct as createVideoStart ('SMsgAVIoctrlAVStream'), just a
        // different sub-command and sequence number.
        static AVPacket createAudioStart(const u8* tokenBytes) {
            return createIoctrlStart(IoctrlType::IPCAM_AUDIO_START, /*frmNo=*/2, tokenBytes);
        }

        static AVPacket createVideoStop() {
            // frame number, obviously wrong, but we'll see if it makes a difference
            // do we have to keep track of the frame numbers? possibly
            return createIoctrlStart(IoctrlType::IPCAM_STOP, /*frmNo=*/3);
        }

        static AVPacket createAudioStop() {
            // again, dont know the frame number, so we are just guessing
            return createIoctrlStart(IoctrlType::IPCAM_AUDIO_STOP, /*frmNo=*/4);
        }

    private:
        // Probably a bit silly to force that but yes
        // also this is a dumb name, we are creating an IOCTRL, not necessarily a 'start'
        static AVPacket createIoctrlStart(u16 ioctlSubCmd, u32 frmNo, const u8* tokenBytes=(u8*)"\x00\x00\x00\x00") {
            AVPacket pkt{};
            pkt.protoHead.type      = AVPacketType::CONTROL;
            pkt.protoHead.opCode    = AVOpCode::IOCTRL_USER;
            pkt.protoHead.ver       = 1;
            pkt.protoHead.payload   = 12;   // 8 bytes cmd + 4 byte token
            pkt.protoHead.frmNo     = frmNo;

            std::array<std::uint8_t, 12> payload{};
            std::memcpy(payload.data(), &ioctlSubCmd, 2);
            std::memcpy(payload.data() + 8, tokenBytes, 4);

            pkt.body.assign(payload.begin(), payload.end());
            return pkt;
        }
    };

    //
    // Frame assembler
    //
    class FrameAssembler
    {
    public:
        FrameAssembler() = default;

        void reset()
        {
            m_fifo.clear();
            m_expectedFrame.reset();
            m_highestSeenFrame.reset();
        }

        std::vector<Frame> processPacket(std::span<const u8> packet)
        {
            if (packet.size() < sizeof(AVPacketHead)) return {};

            AVPacketHead head{};
            std::memcpy(&head, packet.data(), sizeof(head));

            if (head.opCode != AVOpCode::VIDEO_DATA) return {};

            auto payload = packet.subspan(sizeof(head));
            if (payload.size() < head.payload) return {};

            // Auto-sync sequence counters
            if (!m_expectedFrame)
            {
                m_expectedFrame = head.frmNo;
            }
            if (!m_highestSeenFrame || head.frmNo > *m_highestSeenFrame)
            {
                m_highestSeenFrame = head.frmNo;
            }

            Block block{
                .frmNo = head.frmNo,
                .frmSize = head.frmSize,
                .pos = head.packNoInFrm,
                .frmInfoSize = head.frmInfoSize,
                .buffer = std::vector<u8>(payload.begin(), payload.begin() + head.payload)
            };

            m_fifo.push_bypass_same_block(std::move(block));

            std::vector<Frame> readyFrames;
            while (auto frame = tryEmitFrame())
            {
                readyFrames.push_back(std::move(*frame));
            }

            return readyFrames;
        }

    private:
        std::optional<Frame> tryEmitFrame()
        {
            if (!m_expectedFrame) return std::nullopt;

            u32 current = *m_expectedFrame;

            // Handle frame drops: Skip ahead if we've received frames much newer than expected
            if (m_highestSeenFrame && *m_highestSeenFrame > current + 4)
            {
                m_fifo.remove_all_frames_less_than(*m_highestSeenFrame);
                m_expectedFrame = *m_highestSeenFrame;
                current = *m_highestSeenFrame;
            }

            auto info = m_fifo.get_info_by_frm_no(current);
            if (!info) return std::nullopt;

            // Check if all chunks for this frame have arrived
            auto lostPos = m_fifo.get_lost_pos(current);
            if (lostPos.lostFrame || !lostPos.positions.empty())
            {
                // TODO: loss recovery. We know exactly which chunk positions
                // are missing (lostPos.positions) and could send
                // AVOpCode::VIDEO_DATA_REQUEST to ask the camera to resend
                // them - the enum value exists but nothing ever sends it, so
                // right now a dropped UDP packet just stalls this frame
                // until get_lost_pos() gives up or a newer frame arrives and
                // skip-ahead kicks in above. Not implemented because the
                // wire format of a VIDEO_DATA_REQUEST payload (does it carry
                // frmNo + a position list? just frmNo? something else?) was
                // never captured from a real client - guessing at it and
                // sending garbage to the camera seemed worse than leaving
                // this as a known gap. If you get a packet capture of the
                // real TUTK client requesting a retransmit, this is the spot.
                return std::nullopt;
            }

            // Extract all blocks for this frame
            std::vector<Block> blocks;
            while (auto b = m_fifo.pop_by_frm_no(current))
            {
                blocks.push_back(std::move(*b));
            }

            if (blocks.empty()) return std::nullopt;

            const std::size_t totalSize = info->size + info->infoSize;
            std::vector<u8> fullPayload(totalSize, 0);

            // Reconstruct payload from 1024-byte offset blocks
            for (const auto& block : blocks)
            {
                const std::size_t offset = static_cast<std::size_t>(block.pos) * 1024;
                if (offset < fullPayload.size())
                {
                    const std::size_t bytesToCopy = std::min(block.buffer.size(), fullPayload.size() - offset);
                    std::memcpy(fullPayload.data() + offset, block.buffer.data(), bytesToCopy);
                }
            }

            Frame frame;
            frame.frameNo = current;

            // Extract Video Stream
            frame.video.assign(fullPayload.begin(), fullPayload.begin() + info->size);

            // Extract FRAMEINFO struct
            if (info->infoSize >= sizeof(FRAMEINFO))
            {
                std::memcpy(&frame.frameInfo, fullPayload.data() + info->size, sizeof(FRAMEINFO));
            }

            // Advance sequence target
            m_expectedFrame = current + 1;

            return frame;
        }

        BlockFifo m_fifo;
        std::optional<u32> m_expectedFrame;
        std::optional<u32> m_highestSeenFrame;
    };

    // Frame synchronizer
    class FrameSynchronizer
    {
    public:
        void reset()
        {
            m_assembler.reset();
            m_synced = false;
            m_lastFrame = 0;
        }

        std::vector<Frame> processPacket(std::span<const u8> packet)
        {
            std::vector<Frame> delivered;
            auto assembled = m_assembler.processPacket(packet);

            for (auto& frame : assembled)
            {
                if (shouldDeliver(frame))
                {
                    delivered.push_back(std::move(frame));
                }
            }
            return delivered;
        }

    private:
        bool shouldDeliver(const Frame& frame)
        {
            bool isKeyframe = (frame.frameInfo.flags & static_cast<u8>(FrameFlag::IFRAME)) != 0;

            // Must start streaming on an I-Frame (IDR) to prevent decoding artifacts
            if (isKeyframe)
            {
                m_synced = true;
                m_lastFrame = frame.frameNo;
                return true;
            }

            if (!m_synced) return false;

            // A gap here (frame.frameNo > m_lastFrame + 1) means FrameAssembler's
            // loss-skip-ahead already kicked in upstream - it recovered and handed
            // us a real, complete, later frame despite the loss. Deliver it rather
            // than discarding that recovery and freezing until the next keyframe:
            // on a lossy link (UDP - no retransmission implemented, see
            // FrameAssembler::tryEmitFrame's TODO) the old "wait for next IDR"
            // behavior here meant a SINGLE dropped packet froze the whole stream
            // for up to a full GOP length, every time. A gap risks a few frames
            // of decode artifacts until the next keyframe self-heals it - that's
            // the trade-off, and for a live feed it beats a multi-second freeze.
            // frame.frameNo <= m_lastFrame (stale/duplicate) is still dropped.
            if (frame.frameNo <= m_lastFrame)
            {
                return false;
            }
            if (frame.frameNo != m_lastFrame + 1)
            {
                LOG_WARN("video frame gap: expected {}, got {} ({} frame(s) lost) - delivering anyway, may show brief artifacts until next keyframe",
                          m_lastFrame + 1, frame.frameNo, frame.frameNo - m_lastFrame - 1);
            }

            m_lastFrame = frame.frameNo;
            return true;
        }

        FrameAssembler m_assembler;
        bool m_synced{ false };
        u32 m_lastFrame{ 0 };
    };

    // Audio's equivalent of FrameSynchronizer's gap-tracking, minus the
    // keyframe-gating (PCM isn't predictively coded like H.264 - there's no
    // "wait for an IDR" concept, a gap is just a gap). The real TUTK SDK
    // (avRecvAudioData / m_ArrAudioSlot / _Audio_Search_Lost_Data) tracks a
    // per-frame index for audio and explicitly detects/skips missing frames
    // rather than trusting arrival order - our own AUDIO_DATA handling
    // below used to ignore frmNo for audio entirely ("no fifo, no sync"),
    // which meant reordered or duplicate UDP packets would've been played
    // back exactly as they arrived, out of order. This doesn't attempt to
    // recover lost audio (same reason as video: the wire format for
    // requesting a resend was never captured) - it just stops delivering
    // stale/duplicate/reordered chunks and logs when a gap is detected, the
    // same trade-off FrameSynchronizer makes for video.
    class AudioSequencer
    {
    public:
        bool shouldDeliver(u32 frmNo)
        {
            if (!m_lastFrame)
            {
                m_lastFrame = frmNo;
                return true;
            }

            if (frmNo <= *m_lastFrame)
            {
                return false; // stale, duplicate, or arrived out of order
            }

            if (frmNo != *m_lastFrame + 1)
            {
                LOG_WARN("audio frame gap: expected {}, got {} ({} frame(s) lost)",
                          *m_lastFrame + 1, frmNo, frmNo - *m_lastFrame - 1);
            }

            m_lastFrame = frmNo;
            return true;
        }

    private:
        std::optional<u32> m_lastFrame;
    };

    class DualStreamProcessor
    {
    public:
        using AudioCallback = std::function<void(std::span<const u8> pcm, const FRAMEINFO& fi, u32 frmNo)>;
        using VideoCallback = std::function<void(std::span<const u8> h264_frame, const FRAMEINFO& fi)>;

        DualStreamProcessor(VideoCallback videoCB, AudioCallback audioCB)
            : m_videoCallback(std::move(videoCB)), m_audioCallback(std::move(audioCB)) {
        }

        void processPacket(std::span<const u8> rawPacket) {
            if (rawPacket.size() < sizeof(AVPacketHead)) {
                return;
            }

            // Unpack the AVPacketHead
            AVPacketHead head{};
            std::memcpy(&head, rawPacket.data(), sizeof(AVPacketHead));

            auto payload = rawPacket.subspan(sizeof(AVPacketHead));

            // -----------------------------------------------------------
            // AUDIO: no reassembly needed (one packet = one frame already),
            // but now runs through AudioSequencer for ordering/gap
            // detection - see its comment for why.
            // -----------------------------------------------------------
            if (head.opCode == AVOpCode::AUDIO_DATA) {
                if (payload.size() < sizeof(FRAMEINFO)) {
                    return;
                }

                if (!m_audioSync.shouldDeliver(head.frmNo)) {
                    return;
                }

                // extract frameinfo
                FRAMEINFO frameInfo{};
                std::memcpy(&frameInfo, payload.data(), sizeof(frameInfo));

                // extract pcm bytes
                auto dataSpan = payload.subspan(sizeof(frameInfo));
                if (dataSpan.empty()) {
                    return;
                }

                // copy into buffer and swap endianness for RTSP
                std::vector<u8> pcmBuffer(dataSpan.begin(), dataSpan.end());
                swapEndian(pcmBuffer);
                if (m_audioCallback) {
                    m_audioCallback(pcmBuffer, frameInfo, head.frmNo);
                }
            } else if (head.opCode == AVOpCode::VIDEO_DATA) {
                // ----------------------------------------------
                // Video with BlockFifo reassembly, keyframe sync
                // ----------------------------------------------

                auto frames = m_videoSync.processPacket(rawPacket);
                for (const auto& frame : frames) {
                    if (m_videoCallback) {
                        m_videoCallback(frame.video, frame.frameInfo);
                    }
                }
            }

        }

    private:
        // compiler can inline it, likely, should check if we want to force inline
        void swapEndian(std::span<u8> data) {
            auto samples = std::span<u16>(
                reinterpret_cast<u16*>(data.data()),
                data.size() / sizeof(u16)
            );

            for (auto& sample : samples) {
                sample = std::byteswap(sample); // c++23 feature, comes from <bit>
            }
        }

        FrameSynchronizer m_videoSync;
        AudioSequencer m_audioSync;
        VideoCallback m_videoCallback;
        AudioCallback m_audioCallback;
    };

}