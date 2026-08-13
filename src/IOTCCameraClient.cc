#include "IOTCCameraClient.hh"

#define NO_OPENSSL
#include "Live555Bridge.hh"

#include <BasicUsageEnvironment.hh>
#include <RTSPServer.hh>
#include <thread>

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <variant>

struct MediaClock
{
    std::mutex mtx;

    // Set once, from the first accepted video frame (post-keyframe-wait).
    bool initialized{ false };
    std::uint32_t videoBaseTimestampMs{};

    // holy hell LLM nobody wants to read something this long

    // Set once, from the first audio frame delivered after `initialized`
    // becomes true. Audio timestamps are then computed as
    // audioBaseTimestampMs + (frmNo - audioBaseFrmNo) * kAudioFrameIntervalMs
    // - i.e. derived from the sequence number's distance from this anchor,
    // not from how many audio callbacks have fired. That matters now that
    // AudioSequencer (AVAPI.hh) can skip stale/duplicate/lost chunks: if
    // timing were "advance by one interval per delivered chunk" instead, a
    // lost chunk would silently shift every later chunk's timestamp earlier
    // than it should be, then keep silently drifting audio out of sync with
    // video's timeline. Deriving from frmNo directly is immune to that -
    // gaps become correctly-sized gaps in time, not gaps in the exact
    // 40ms-per-slot correspondence.

    bool audioAnchored{ false };
    std::uint32_t audioBaseFrmNo{};
    std::uint32_t audioBaseTimestampMs{};
    std::uint32_t nextAudioTimestampMs{};
};

void print_header(const iotc::IOTCHeader& h)
{
    LOG_DEBUG("magic:        {:#x}", h.magic);
    LOG_DEBUG("version:      {:#x}", h.version);
    LOG_DEBUG("flags1:       {:#x}", h.flags1);
    LOG_DEBUG("length:       {:#x}", h.length);
    LOG_DEBUG("packetCount:  {:#x}", h.packetCount);
    LOG_DEBUG("type:         {:#x}", (std::uint16_t)h.type);
    LOG_DEBUG("flags2:       {:#x}", h.flags2);
    LOG_DEBUG("RLYsessionId: {:#x}", h.rlySessionId);
    LOG_DEBUG("channelId:    {:#x}", h.chId);
    LOG_DEBUG("reserved:     {:#x}", h.reserved);
}

std::string_view getAVPacketOpcode(const AVAPI::AVOpCode& op)
{
    switch (op)
    {
    case AVAPI::AVOpCode::AV_LOGIN:
        return "AV_LOGIN";
    case AVAPI::AVOpCode::AV_LOGIN_RESP:
        return "AV_LOGIN_RESP";
    case AVAPI::AVOpCode::VIDEO_DATA:
        return "VIDEO_DATA";
    case AVAPI::AVOpCode::AUDIO_DATA:
        return "AUDIO_DATA";
    case AVAPI::AVOpCode::VIDEO_DATA_RESEND:
        return "VIDEO_DATA_RESEND";
    case AVAPI::AVOpCode::AUDIO_DATA_RESEND:
        return "AUDIO_DATA_RESEND";
    case AVAPI::AVOpCode::VIDEO_DATA_REQUEST:
        return "VIDEO_DATA_REQUEST";
    case AVAPI::AVOpCode::VIDEO_DATA_OK:
        return "VIDEO_DATA_OK";
    case AVAPI::AVOpCode::VIDEO_DATA_OK_ACK:
        return "VIDEO_DATA_OK_ACK";
    case AVAPI::AVOpCode::VIDEO_DATA_LOST:
        return "VIDEO_DATA_LOST";
    case AVAPI::AVOpCode::IOCTRL_INNER:
        return "IOCTRL_INNER";
    case AVAPI::AVOpCode::IOCTRL_INNER_ACK:
        return "IOCTRL_INNER_ACK";
    case AVAPI::AVOpCode::IOCTRL_USER:
        return "IOCTRL_USER";
    case AVAPI::AVOpCode::IOCTRL_USER_ACK:
        return "IOCTRL_USER_ACK";
    default:
        return "UNKNOWN_OPCODE";
    }
}

void print_avpacket_header(const AVAPI::AVPacketHead& p)
{
    LOG_DEBUG("AVPacket type:        {:#x}", (u8)p.type);
    LOG_DEBUG("AVPacket opcode:      {}", getAVPacketOpcode(p.opCode));
    LOG_DEBUG("AVPacket ver:         {:#x}", p.ver);
    LOG_DEBUG("AVPacket frmNo:       {:#x}", p.frmNo);
    LOG_DEBUG("AVPacket frmSize:     {:#x}", p.frmSize);
    LOG_DEBUG("AVPacket packNoInFrm: {:#x}", p.packNoInFrm);
    LOG_DEBUG("AVPacket frmInfoSize: {:#x}", p.frmInfoSize);
    if (p.payload > 1400 || p.payload == 0) {
        LOG_DEBUG("AVPacket payload:     {:#x} (NOT SENSICAL)", p.payload);
    } else {
        LOG_DEBUG("AVPacket payload:     {:#x}", p.payload);
    }
    // p.reserve1 is a raw uint8_t[6] - std::format has no formatter for that
    // (it was previously passed straight to "{}", which wouldn't compile).
    std::string reserve1Hex;
    for (auto b : p.reserve1) {
        reserve1Hex += std::format("{:02x} ", b);
    }
    LOG_DEBUG("AVPacket reserve1:    {}", reserve1Hex);
}

void hexDump(const std::string_view label, const uint8_t* data, size_t size) {
    std::string hex;
    for (size_t i = 0; i < size; ++i) {
        hex += std::format("{:02X} ", data[i]);
    }
    LOG_DEBUG("{} ({} bytes): {}", label, size, hex);
}

// Debug helper: dumps a received packet's header and (if it looks like an
// AVPacket) its AV header + raw body. Called once per session while waiting
// for the AV login response's token bytes - fine as a one-shot, would be
// noisy if called on every packet in the streaming loop.
void decrypt_packet(const iotc::IOTCPacket& p)
{
    print_header(p.header);
    if (p.header.type == iotc::IotcPacketType::DataSessionResp && p.payload.size() >= sizeof(AVAPI::AVPacketHead)) {
        AVAPI::AVPacketHead head;
        std::memcpy(&head, p.payload.data(), sizeof(AVAPI::AVPacketHead));

        bool bodyLooksValid = head.payload != 0 && head.payload <= 1400
            && p.payload.size() >= sizeof(AVAPI::AVPacketHead) + head.payload;

        if (!bodyLooksValid) {
            LOG_WARN("decrypt_packet(): AVPacket payload is {}, wont hexdump the body", head.payload);
        }

        print_avpacket_header(head);

        if (bodyLooksValid) {
            std::vector<u8> data(head.payload, 0);
            std::memcpy(data.data(), p.payload.data() + sizeof(AVAPI::AVPacketHead), head.payload);
            hexDump("AVPacketBody", data.data(), data.size());
        }
    }
    hexDump("raw data", p.payload.data(), p.header.length);
}

IOTCCameraClient::IOTCCameraClient(asio::io_context& ioctx, std::string uid, FrameCallback onFrameCB,
                                    IOTCCameraConfig config)
    : m_socket(ioctx), m_timer(ioctx), m_uid(std::move(uid)), m_frameCB(std::move(onFrameCB)),
      m_config(std::move(config)) {
    m_socket.open(udp::v4());
    m_socket.bind(udp::endpoint(asio::ip::make_address_v4(m_config.bindIp), 0));
}

// spawn coroutine(s)
void IOTCCameraClient::start()
{
    asio::co_spawn(m_socket.get_executor(),
        [self = shared_from_this()]() -> asio::awaitable<void> {
            co_await self->run_with_reconnect();
        },
        asio::detached);
}

// kill
asio::awaitable<void> IOTCCameraClient::stop()
{
    if (m_isStopping) {
        co_return;
    }
    m_isStopping = true;

    // this is stupid we dont even throw in our send_packet
    try {
        auto audioStopPkt = AVAPI::AVPacketFactory::createAudioStop();
        auto videoStopPkt = AVAPI::AVPacketFactory::createVideoStop();
        // Send IOCTRL to STOP the stream, maybe stop the audio stream as well

        if (m_config.enableAudio) {
            co_await send_packet(iotc::IotcPacketType::DataSessionReq, audioStopPkt.serialize());
            //co_await receive_packet_with_timeout();
        }

        co_await send_packet(iotc::IotcPacketType::DataSessionReq, videoStopPkt.serialize());
        //co_await receive_packet_with_timeout();

        // Also we have to do what 'IOTC_P2P_Close' does, so that looks like a pretty raw packet
        // Depending on 'client or device' flag
        // if (v4->CorD)
        // 0x418 + 0x12, or
        // 0x417 + 0x21
        // wait, this just looks like a regular IOTC packet, there is no message?
        // We would want 'clientRandomID' but its not necessarily set, probably its zero too
        // so what? Its a null packet?
        // TODO: of course, we should be constructing the packet in our 'Factory' class, but i dont care enough rigght now
        
        const auto timeout = std::chrono::milliseconds(1000);
        co_await send_packet(iotc::IotcPacketType::CloseSessionReq1, {});
        //co_await wait_for_packet_type(iotc::IotcPacketType::CloseSessionReq2, timeout);
        //co_await wait_for_packet_type(iotc::IotcPacketType::CloseSessionAck, timeout);
    }
    catch (const std::exception& e) {
        std::println("Error sending teardown packet: {}", e.what());
    }

    // Close the socket to cancel pending async operations (like receive_packet)
    //boost::system::error_code ec;
    //m_socket.close(ec);
    //exit(1); // Get the fuck out of here asap
}

// Wraps session_state_machine() with exponential backoff so one failed
// attempt (dropped LAN search, camera reboot, whatever) doesn't kill the
// bridge for good. session_state_machine() already catches its own
// exceptions and just returns, so from here every return looks the same
// whether it was a clean end or an error - that's fine, we just retry
// either way. Backoff resets to the floor once a session has stayed up
// longer than kHealthySessionThreshold, so a camera that drops out once an
// hour doesn't end up waiting a full minute to reconnect.
asio::awaitable<void> IOTCCameraClient::run_with_reconnect()
{
    constexpr auto kInitialBackoff = std::chrono::seconds(1);
    constexpr auto kMaxBackoff = std::chrono::seconds(30);
    constexpr auto kHealthySessionThreshold = std::chrono::seconds(60);

    auto backoff = kInitialBackoff;

    while (true) {
        auto attemptStart = std::chrono::steady_clock::now();

        co_await session_state_machine();

        auto elapsed = std::chrono::steady_clock::now() - attemptStart;
        if (elapsed >= kHealthySessionThreshold) {
            backoff = kInitialBackoff;
        }

        if (m_isStopping) {
            co_return;
        }

        LOG_WARN("[{}] session ended after {}s, reconnecting in {}s",
                 m_uid, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), backoff.count());

        // reset per-session state so the next LAN search starts clean
        m_cameraEndpoint = udp::endpoint{};

        asio::steady_timer backoffTimer(m_socket.get_executor());
        backoffTimer.expires_after(backoff);
        co_await backoffTimer.async_wait(asio::use_awaitable);

        backoff = std::min(backoff * 2, kMaxBackoff);
    }
}

// Async send a packet
// Serializes a IOTC packet(!)
asio::awaitable<void> IOTCCameraClient::send_packet(iotc::IotcPacketType type, std::span<const u8> payload, udp::endpoint target, u16 packetCountOverride)
{
    if (target == udp::endpoint{}) {
        target = m_cameraEndpoint;
    }

    iotc::IOTCHeader header{};
    header.type = type;
    header.length = static_cast<u16>(payload.size());
    header.rlySessionId = m_currentSessionId;
    header.flags1 = 2;
    header.flags2 = 0x21; // common default - meaning not reverse engineered
    header.version = 6;   // observed constant across every packet type
    if (packetCountOverride != 0) {
        header.packetCount = packetCountOverride;
    }

    // Type=0x401 (P2PKnock) and 0x402 (P2PKnockResp) are the exception and
    // always go out with flags2=0x233 instead of the 0x21 default above.
    switch (type) {
        case iotc::IotcPacketType::P2PKnock:
            header.flags2 = 0x233;
            header.reserved = 1;
            break;
        case iotc::IotcPacketType::P2PKnockResp:
            header.flags2 = 0x233;
            break;
        case iotc::IotcPacketType::DataSessionReq:
            header.rlySessionId = 0x7195; // observed constant relay session id for DataSessionReq specifically - purpose unclear, camera doesn't seem to care what it actually is
            break;
        case iotc::IotcPacketType::CloseSessionReq1: // Packet is pretty much just zeroes, honestly
            break;
        default:
            break;
    }

    auto tmpBuffer = iotc::IOTCPacket::serialize(header, payload);
    std::vector<u8> packetBuffer(tmpBuffer.size());
    Scrambler::TransCodePartial(tmpBuffer, packetBuffer, tmpBuffer.size());

    co_await m_socket.async_send_to(asio::buffer(packetBuffer), target, asio::use_awaitable);
}

// Async receive and deserialize a packet
asio::awaitable<iotc::IOTCPacket> IOTCCameraClient::receive_packet()
{
    std::vector<u8> recvBuffer(2048); // real-world max is ~1500 (MTU), rounded up for headroom
    udp::endpoint senderEp;

    while (true)
    {
        std::size_t bytesRead = co_await m_socket.async_receive_from(
            asio::buffer(recvBuffer), senderEp, asio::use_awaitable
        );

        if (bytesRead < 16) {
            continue; // too small to be a real packet
        }

        // Descramble only the bytes we actually received - passing the
        // recvBuffer's full capacity here would run the descrambler over
        // uninitialized tail bytes.
        std::vector<u8> buffer(bytesRead);
        Scrambler::ReverseTransCodePartial(
            std::span<const u8>(recvBuffer.data(), bytesRead),
            buffer,
            bytesRead);

        iotc::IOTCPacket pkt;
        if (iotc::IOTCPacket::deserialize(buffer, pkt))
        {
            if (m_cameraEndpoint == udp::endpoint{})
                m_cameraEndpoint = senderEp;
            co_return pkt;
        }
    }
}

asio::awaitable<iotc::IOTCPacket> IOTCCameraClient::receive_packet_with_timeout(std::chrono::milliseconds timeout_duration)
{
    using namespace asio::experimental::awaitable_operators;

    asio::steady_timer timer(m_socket.get_executor(), timeout_duration);

    // The '||' operator races the two awaitables.
    // It returns a std::variant based on which one completed first.
    auto result = co_await(receive_packet() || timer.async_wait(asio::use_awaitable));

    // result.index() == 1 means the timer finished first (timeout)
    if (result.index() == 1) {
        throw std::runtime_error("Timeout waiting for packet");
    }

    // Otherwise, extract and return the packet
    co_return std::get<0>(std::move(result));
}

// keep-alive
asio::awaitable<void> IOTCCameraClient::keep_alive()
{
    try {
        while (true) {
            if (m_isStopping) {
                co_return;
            }
            m_timer.expires_after(std::chrono::seconds(3));
            co_await m_timer.async_wait(asio::use_awaitable);
            co_await send_packet(iotc::IotcPacketType::KeepaliveReq);
            //co_await receive_packet(); // Receive the D2C
        }
    }
    catch (const std::exception& e) {
        LOG_WARN("Keep-alive stopped, because {}", e.what());
    }
}

asio::awaitable<iotc::IOTCPacket> IOTCCameraClient::wait_for_packet(
    std::function<bool(const iotc::IOTCPacket&)> predicate,
    std::string what,
    std::chrono::milliseconds overallTimeout)
{
    auto deadline = std::chrono::steady_clock::now() + overallTimeout;

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            throw std::runtime_error(std::format("timed out waiting for {}", what));
        }

        // Cap the individual receive at the remaining phase budget so a
        // steady trickle of non-matching packets (stray keepalives, a
        // retransmitted knock from a previous attempt, whatever) can't keep
        // resetting a fresh per-call timeout forever - the outer deadline
        // above is what actually bounds this loop.
        auto pkt = co_await receive_packet_with_timeout(remaining);
        if (predicate(pkt)) {
            co_return pkt;
        }
        LOG_DEBUG("wait_for_packet({}): ignoring unrelated packet type {:#x}", what, (u16)pkt.header.type);
    }
}

asio::awaitable<iotc::IOTCPacket> IOTCCameraClient::wait_for_packet_type(
    iotc::IotcPacketType expected, std::chrono::milliseconds overallTimeout)
{
    return wait_for_packet(
        [expected](const iotc::IOTCPacket& p) { return p.header.type == expected; },
        std::format("packet type {:#x}", (u16)expected),
        overallTimeout);
}

asio::awaitable<void> IOTCCameraClient::session_state_machine()
{
    try {
        // Broadcast is only needed for the initial LAN search, then off.
        m_socket.set_option(asio::socket_base::broadcast(true));

        //
        // PHASE 1: LAN Search (Broadcast, find uid, get reflected IP+PORT)
        //
        auto searchData = iotc::IOTCPacketFactory::createLanSearch(m_uid);
        udp::endpoint broadcast_ep(asio::ip::make_address("255.255.255.255"), iotc::IOTC_DISCOVERY_PORT);
        co_await send_packet(iotc::IotcPacketType::LanSearchReq, searchData, broadcast_ep);

        auto lanResp = co_await wait_for_packet_type(iotc::IotcPacketType::LanSearchResp, m_config.handshakeTimeout);
        if (lanResp.payload.size() < 28) {
            throw std::runtime_error("LAN search response too short");
        }

        u16 reflectedPort = 0;
        std::memcpy(&reflectedPort, lanResp.payload.data() + 22, 2);

        std::array<u8, 4> ipBytes{};
        std::memcpy(ipBytes.data(), lanResp.payload.data() + 24, 4);
        auto reflectedIp = boost::asio::ip::make_address_v4(ipBytes);

        // no broadcasting now
        m_socket.set_option(asio::socket_base::broadcast(false));

        //
        // PHASE 2: Hole punch, dont even bother checking for response
        //
        auto punchData = iotc::IOTCPacketFactory::createNatPunch(m_uid, reflectedIp, reflectedPort);
        co_await send_packet(iotc::IotcPacketType::NatPunch, punchData);

        //
        // PHASE 3: P2P Knock (handshaking, idk)
        //
        auto knockData = iotc::IOTCPacketFactory::createKnock(m_uid);
        co_await send_packet(iotc::IotcPacketType::P2PKnock, knockData);
        co_await wait_for_packet_type(iotc::IotcPacketType::P2PKnock, m_config.handshakeTimeout);

        co_await send_packet(iotc::IotcPacketType::P2PKnockResp, knockData);
        co_await wait_for_packet_type(iotc::IotcPacketType::P2PKnockResp, m_config.handshakeTimeout);
        co_await wait_for_packet_type(iotc::IotcPacketType::P2PKnockRespResp, m_config.handshakeTimeout);

        //
        // PHASE 4: AV Session Login & streaming
        auto avLoginPkt = AVAPI::AVPacketFactory::createLogin(m_config.camUsername, m_config.camPassword);
        co_await send_packet(iotc::IotcPacketType::DataSessionReq, avLoginPkt.serialize());

        // Trailing 4 bytes of the AV_LOGIN_RESP body, echoed back in the
        // video/audio start IOCTRLs. Purpose not confirmed - see createVideoStart().
        auto d2c_resp = co_await wait_for_packet(
            [](const iotc::IOTCPacket& p) {
                return p.header.type == iotc::IotcPacketType::DataSessionResp && p.payload.size() >= 4;
            },
            "AV login response", m_config.handshakeTimeout);
        decrypt_packet(d2c_resp);

        u8 tokenBytes[4]{};
        std::memcpy(tokenBytes, d2c_resp.payload.data() + (d2c_resp.payload.size() - 4), 4);

        auto videoReqPkt = AVAPI::AVPacketFactory::createVideoStart(tokenBytes);
        co_await send_packet(iotc::IotcPacketType::DataSessionReq, videoReqPkt.serialize());

        if (m_config.enableAudio) {
            auto audioReqPkt = AVAPI::AVPacketFactory::createAudioStart(tokenBytes);
            co_await send_packet(iotc::IotcPacketType::DataSessionReq, audioReqPkt.serialize(), {}, /*packetCountOverride=*/2);
        }


        // start sending keepalive
        asio::co_spawn(m_socket.get_executor(),
            [self = shared_from_this()] -> asio::awaitable<void> {
                co_await self->keep_alive();
            }, asio::detached
        );

        // PHASE 5: Receive data forever, decode into RTSP via live555.

        // SETUP LIVE555
        // OutPacketBuffer::maxSize when creating new subsession
        TaskScheduler* scheduler = BasicTaskScheduler::createNew();
        UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);


        // Opt-in RTSP auth: leave m_config.rtspUsername/rtspPassword empty for
        // an open server (fine on a trusted LAN, not otherwise - see the TODO
        // on IOTCCameraConfig). authDB is intentionally leaked here, same
        // lifetime as the rest of this live555 setup (env/scheduler aren't
        // cleaned up either - this whole function only returns on error).
        UserAuthenticationDatabase* authDB = nullptr;
        if (!m_config.rtspUsername.empty()) {
            authDB = new UserAuthenticationDatabase();
            authDB->addUserRecord(m_config.rtspUsername.c_str(), m_config.rtspPassword.c_str());
        }
        else {
            LOG_WARN("no RTSP username configured - server is running with no auth, anyone who can reach port {} gets the stream", m_config.rtspPort);
        }

        RTSPServer* rtspServer = RTSPServer::createNew(*env, m_config.rtspPort, authDB);
        if (!rtspServer) {
            throw std::runtime_error(std::format("Failed to start RTSP Server: {}\n", env->getResultMsg()));
        }

        ServerMediaSession* sms = ServerMediaSession::createNew(*env, "live", "TUTK AV Stream", "Live Video & Audio");

        auto videoQueue = std::make_shared<FrameQueue>();
        auto audioQueue = std::make_shared<FrameQueue>();
        auto mediaClock = std::make_shared<MediaClock>();

        // Video subsession
        sms->addSubsession(TutkServerMediaSubsession::createNew(*env, *videoQueue));
        //sms->addSubsession(TutkServerMediaSubsession::createNew(*env, videoBroadcaster));

        // Audio subsession (PCM 16-bit LE -> BE, 8KHz mono)
        if (m_config.enableAudio) { sms->addSubsession(TutkAudioServerMediaSubsession::createNew(*env, *audioQueue, 8000, 1)); }
        //if (m_config.enableAudio) { sms->addSubsession(TutkAudioServerMediaSubsession::createNew(*env, audioBroadcaster, 8000, 1)); }


        rtspServer->addServerMediaSession(sms);
        char* url = rtspServer->rtspURL(sms);
        LOG_INFO("RTSP Stream is ready: {}", url);
        delete[] url;

        std::thread live555thread([env]() {
            env->taskScheduler().doEventLoop();
        });
        live555thread.detach();

        bool wait_for_iframe = true;

        AVAPI::DualStreamProcessor proc(
            [&wait_for_iframe, videoQueue, mediaClock](std::span<const u8> data, const AVAPI::FRAMEINFO& fi) {
                // Find NAL unit type
                uint8_t nal = 0;
                if (data.size() > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
                    nal = data[4] & 0x1F;
                }
                else if (data.size() > 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
                    nal = data[3] & 0x1F;
                }

                // 7 = SPS, 5 = IDR (I-Frames)
                if (wait_for_iframe) {
                    if (nal == 7 || nal == 5) {
                        wait_for_iframe = false; // We found the keyframe! Allow frames through.
                        LOG_INFO("Found first I-Frame. Starting video feed.");
                    }
                    else {
                        return; // Drop P-frame
                    }
                }

                // Anchor the shared audio/video clock to this stream's first
                // accepted frame - audio chunks (below) get timestamped
                // relative to this rather than trusting their own per-packet
                // timestamp, since audio gets no reassembly/sync at all on
                // the wire (see AVAPI.hh's DualStreamProcessor comment).
                {
                    std::lock_guard<std::mutex> lock(mediaClock->mtx);
                    if (!mediaClock->initialized) {
                        mediaClock->initialized = true;
                        mediaClock->videoBaseTimestampMs = fi.timestamp; // ARBITRARY EPOCH i guess
                        mediaClock->nextAudioTimestampMs = fi.timestamp;
                    }
                }


                // actually, the hardware timestamp sucks loel
                // its all over the place, +40 +40 +40 +60 +70 +40, so it cant be used for this purpose
                // LOG_INFO("Video TS: {}", fi.timestamp);
                videoQueue->push(
                    std::vector<std::uint8_t>(data.begin(), data.end()),
                    fi.timestamp,
                    false
                );
            },
            [audioQueue, mediaClock](std::span<const u8> data, const AVAPI::FRAMEINFO&, std::uint32_t frmNo) {
                // Confirmed fixed cadence (see Live555Bridge.hh's
                // kAudioFrameIntervalMs): every audio chunk is exactly one
                // interval after the last *by sequence number*, so derive
                // timing from frmNo's distance from a fixed anchor rather
                // than from arrival time, byte counts, or a plain
                // per-callback counter (see MediaClock's comment on why the
                // last one specifically is wrong now that lossy/reordered
                // chunks get dropped upstream instead of all being
                // delivered 1:1). Dropped entirely until the video side has
                // set the clock's starting point - audio with no
                // video-relative base to anchor to isn't useful to send.
                std::uint32_t audioTimestampMs;
                {
                    std::lock_guard<std::mutex> lock(mediaClock->mtx);
                    if (!mediaClock->initialized) {
                        return;
                    }

                    if (!mediaClock->audioAnchored) {
                        mediaClock->audioAnchored = true;
                        mediaClock->audioBaseFrmNo = frmNo;
                        mediaClock->audioBaseTimestampMs = mediaClock->videoBaseTimestampMs;
                    }

                    // AudioSequencer upstream already guarantees frmNo only
                    // increases across delivered chunks, so this subtraction
                    // never underflows.
                    std::uint32_t elapsedSlots = frmNo - mediaClock->audioBaseFrmNo;
                    audioTimestampMs = mediaClock->audioBaseTimestampMs + elapsedSlots * kAudioFrameIntervalMs;
                }
                //LOG_INFO("AUDIO | Real System Delta: {} ms | Calculated Audio TS: {} ms", elapsedMs, audioTimestampMs);

                audioQueue->push(
                    std::vector<std::uint8_t>(data.begin(), data.end()),
                    audioTimestampMs,
                    true
                );
            }
        );

        // TODO: add a timeout here too - keepalive will keep going even if the
        // camera has stopped sending DataSessionResp packets.
        while (true) {
            auto pkt = co_await receive_packet_with_timeout();

            if (pkt.header.type == iotc::IotcPacketType::DataSessionResp) {
                proc.processPacket(pkt.payload);
            }
        }
    } catch (const std::exception& e) {
        if (m_isStopping) {
            co_return;
        }
        LOG_ERROR("State machine stopped: {}", e.what());
    } catch (...) {
        LOG_ERROR("State machine stopped, error not available");
    }
}