#include "IOTCCameraClient.hh"

#include <boost/program_options.hpp>    // YEAH
#include <boost/asio/signal_set.hpp>    // we want a clean exit
#include <iostream> // its unfortunate, but program_options wont go with <print>

#if defined(_WIN32) || defined(_WIN64)
    #include <WinSock2.h>
    #include <WS2tcpip.h>
    #include <stdexcept>
    #include <format>
#endif

// Folded in from WinsockGuard.hh - only consumer, and small enough that a
// separate file for it was more indirection than it was worth.
class WinsockGuard
{
public:
    WinsockGuard() {
#if defined(_WIN32) || defined(_WIN64)
        WSADATA wsaData{};
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            throw std::runtime_error(
                std::format("WSAStartup failed (error: {})", result)
            );
        }
#endif
    }

    ~WinsockGuard() {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
    }

    // NO COPY NO MOVE
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
    WinsockGuard(WinsockGuard&&) = delete;
    WinsockGuard& operator=(WinsockGuard&&) = delete;
};

namespace {
    logging::Level parseLogLevel(const std::string& s) {
        if (s == "debug") return logging::Level::Debug;
        if (s == "info")  return logging::Level::Info;
        if (s == "warn")  return logging::Level::Warn;
        if (s == "error") return logging::Level::Error;
        throw std::runtime_error(std::format("unknown --log-level '{}' (expected debug|info|warn|error)", s));
    }
}

// boost program options
bool parseArgs(int argc, char** argv, std::string& uid, IOTCCameraConfig& config);

int main(int argc, char** argv)
{
    std::string         uid;
    IOTCCameraConfig    config;

    if (!parseArgs(argc, argv, uid, config)) {
        return 1;
    }

    WinsockGuard wsGuard;
    try {
        asio::io_context ioc;

        auto on_frame = [](std::span<const u8> frame) {
            std::println("Received a H264 frame, size = {}", frame.size());
        };

        auto client = std::make_shared<IOTCCameraClient>(ioc, uid, on_frame, config);
        client->start();

        // Well, we just a need a 'stop' method, easy!
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);

        signals.async_wait([&ioc, client](const boost::system::error_code& ec, int sig) {
            std::println("Received signal ({})", sig);
            std::println("Sending STOP IOCTRL to device, etc");
            asio::co_spawn(ioc, [client]() -> asio::awaitable<void> {
                co_await client->stop();
            }, asio::detached);
        });

        ioc.run();
    } catch (const std::exception& e) {
        std::println("main: {}", e.what());
        return 1;
    } catch (...) {
        std::println("main: unknown error");
        return 1;
    }

    return 0;
}

// Parses argv into a camera uid + IOTCCameraConfig. Returns false if the
// program should exit immediately (--help, or a parse error already
// printed to stderr) - main() checks that and returns without doing
// anything else.
bool parseArgs(int argc, char** argv, std::string& uid, IOTCCameraConfig& config)
{
    namespace po = boost::program_options;
    // Seed CLI defaults from the struct's own defaults rather than
    // hardcoding them a second time here - keeps --help accurate if
    // IOTCCameraConfig's defaults (IOTCCameraClient.hh) ever change.
    unsigned handshakeTimeoutMs = static_cast<unsigned>(config.handshakeTimeout.count());
    std::string logLevelStr = "info";

    po::options_description desc(
        "tutk_rtsp_bridge - bridges a TUTK/IOTC camera to RTSP\n\n"
        "Usage: tutk_rtsp_bridge <uid> [options]\n"
        "       tutk_rtsp_bridge --uid <uid> [options]\n\n"
        "Options"
    );
    desc.add_options()
        ("help,h", "print this help message and exit")
        ("uid", po::value(&uid),
            "camera's TUTK UID (required - can also be given positionally)")
        ("bind-ip", po::value(&config.bindIp)->default_value(config.bindIp),
            "local interface to bind to (0.0.0.0 = all interfaces)")
        ("cam-user", po::value(&config.camUsername)->default_value(config.camUsername),
            "camera login username")
        ("cam-pass", po::value(&config.camPassword)->default_value(config.camPassword),
            "camera login password - NOTE: visible in `ps`/shell history on this "
            "and every --*-pass option below; fine for a quick local run, not for "
            "anything shared")
        ("rtsp-port", po::value(&config.rtspPort)->default_value(config.rtspPort),
            "port the RTSP server listens on")
        ("rtsp-user", po::value(&config.rtspUsername)
            ->default_value(config.rtspUsername, "none - RTSP server runs unauthenticated"),
            "RTSP auth username (leave unset for an open server - fine on a trusted LAN only)")
        ("rtsp-pass", po::value(&config.rtspPassword)->default_value(config.rtspPassword),
            "RTSP auth password (only meaningful if --rtsp-user is set)")
        ("handshake-timeout-ms", po::value(&handshakeTimeoutMs)->default_value(handshakeTimeoutMs),
            "overall deadline per handshake phase - see IOTCCameraConfig::handshakeTimeout "
            "for why this exists (UDP loss during the LAN search/knock sequence)")
        ("audio", po::value(&config.enableAudio)->default_value(false),
            "should we enable audio? can be fucky")
        ("log-level", po::value(&logLevelStr)->default_value(logLevelStr),
            "debug|info|warn|error")
        ;

    po::positional_options_description pos;
    pos.add("uid", 1);

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return false;
        }

        po::notify(vm);

        if (uid.empty()) {
            std::cerr << "error: no uid given (positionally or via --uid)\n\n" << desc << "\n";
            return false;
        }

        config.handshakeTimeout = std::chrono::milliseconds(handshakeTimeoutMs);
        logging::set_level(parseLogLevel(logLevelStr));
    }
    catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n\n" << desc << "\n";
        return false;
    }

    return true;
}
