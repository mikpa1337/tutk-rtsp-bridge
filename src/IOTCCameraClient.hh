#pragma once

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <print>
#include <vector>
#include <span>
#include <functional>
#include <fstream>
#include <memory>
#include <chrono>
#include <string_view>

#include "IOTCPacket.hh"
#include "AVAPI.hh"
#include "scrambler.hh"
#include "Logger.hh"

namespace asio = boost::asio;
using boost::asio::ip::udp;
using u8  = std::uint8_t;
using u16 = std::uint16_t;

// Config bundle so credentials/ports/bind address aren't compiled-in magic
// values scattered through the .cc file. Still no actual secret storage
// (env vars, a config file, whatever) wired up at the call site - that's on
// whoever constructs this. See the constructor's default arguments for
// what used to be hardcoded here.
struct IOTCCameraConfig
{
    // "0.0.0.0" binds to all interfaces and lets the OS pick the right one
    // per-route on send - the correct portable default. A hardcoded specific
    // address (what this used to default to) only works on the one machine
    // that address happens to belong to; set this explicitly only if you
    // actually need to pin to one NIC on a multi-homed box.
    std::string bindIp = "0.0.0.0";
    std::string camUsername = "admin";
    std::string camPassword = "00000000";

    std::uint16_t rtspPort = 8554;
    // RTSP auth is opt-in: leave both empty for an open server (fine on a
    // trusted LAN, not otherwise). TODO: move these out of compiled-in
    // defaults and into an env var / config file before using this
    // anywhere less trusted than your own LAN.
    std::string rtspUsername;
    std::string rtspPassword;

    // Overall deadline for each handshake phase (LAN search, each knock
    // step, AV login) - NOT the same as receive_packet_with_timeout's
    // per-call timeout. UDP means a stray/retransmitted/leftover packet
    // (e.g. a keepalive from a half-completed earlier attempt) can keep
    // arriving and resetting the per-call timeout forever without ever
    // being the packet a phase is actually waiting for. This bounds the
    // whole phase regardless of how many non-matching packets show up in
    // the meantime, so a stuck handshake surfaces as a timeout and gets
    // picked up by run_with_reconnect() instead of hanging forever.
    std::chrono::milliseconds handshakeTimeout = std::chrono::seconds(5);

    // audio can be fucky
    bool enableAudio;
};

class IOTCCameraClient : public std::enable_shared_from_this<IOTCCameraClient>
{
public:
    using FrameCallback = std::function<void(std::span<const u8> h264_frame)>;

    IOTCCameraClient(asio::io_context& ioctx, std::string uid, FrameCallback onFrameCB,
                      IOTCCameraConfig config = {});

    // Spawns the reconnect-supervised session coroutine on the socket's executor.
    void start();
    
    // This will send STOP ioctrl and whatever is necessary for a graceful shutdown
    asio::awaitable<void> stop();

private:
    bool m_isStopping{ false };
    udp::socket m_socket;
    asio::steady_timer m_timer;
    std::string m_uid;
    udp::endpoint m_cameraEndpoint;
    FrameCallback m_frameCB;
    u16 m_currentSessionId{};
    IOTCCameraConfig m_config;

    // Async send a packet. packetCountOverride, when nonzero, is written
    // into header.packetCount - this replaces the old wantAVFrame bool hack
    // that was only ever used to special-case one audio-frame marker.
    asio::awaitable<void> send_packet(iotc::IotcPacketType type, std::span<const u8> payload = {},
                                       udp::endpoint target = {}, u16 packetCountOverride = 0);

    // Async receive and deserialize a packet
    asio::awaitable<iotc::IOTCPacket> receive_packet();

    // A way to not loop forever
    asio::awaitable<iotc::IOTCPacket> receive_packet_with_timeout(std::chrono::milliseconds timeout_duration = std::chrono::milliseconds(2000));

    // Loops receive_packet_with_timeout() until a packet matching `predicate`
    // arrives, discarding (and LOG_DEBUG-ing) everything else - but bounded
    // by an overall deadline rather than resetting on every non-matching
    // packet. See IOTCCameraConfig::handshakeTimeout for why this exists.
    // `what` is just for the timeout error message - takes std::string (not
    // string_view) on purpose: this is a coroutine, so `what` lives in its
    // frame across suspension points, and a view into a caller-side
    // temporary (e.g. from std::format) would dangle by the time we resume.
    asio::awaitable<iotc::IOTCPacket> wait_for_packet(
        std::function<bool(const iotc::IOTCPacket&)> predicate,
        std::string what,
        std::chrono::milliseconds overallTimeout);

    // Convenience wrapper for the common "just match on header.type" case.
    asio::awaitable<iotc::IOTCPacket> wait_for_packet_type(
        iotc::IotcPacketType expected, std::chrono::milliseconds overallTimeout);

    // keep-alive
    asio::awaitable<void> keep_alive();

    // Drives one LAN-search -> hole-punch -> AV-login -> streaming attempt.
    // Returning (rather than throwing) means "that attempt ended" - could be
    // a clean shutdown or an error, run_with_reconnect() doesn't distinguish.
    asio::awaitable<void> session_state_machine();

    // Wraps session_state_machine() in a reconnect loop with exponential
    // backoff (capped), so a dropped LAN search or a camera reboot doesn't
    // kill the bridge permanently. Backoff resets after a session that
    // stayed up longer than kHealthySessionThreshold.
    asio::awaitable<void> run_with_reconnect();
};