#pragma once

// Minimal leveled logging so debug-only chatter (hex dumps, per-packet
// headers) can be silenced at runtime without deleting the print statements
// that were useful while reverse engineering this. Not fancy on purpose -
// no sinks, no async queue, just a level filter around std::println.

#include <print>
#include <format>
#include <atomic>

namespace logging {

    enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

    // Default to Info - flip to Debug (e.g. at the top of main()) when you
    // need the packet-level hexdumps back.
    inline std::atomic<Level> g_level{Level::Info};

    inline void set_level(Level lvl) { g_level.store(lvl, std::memory_order_relaxed); }

    inline const char* prefix(Level lvl) {
        switch (lvl) {
            case Level::Debug: return "[DEBUG]";
            case Level::Info:  return "[INFO ]";
            case Level::Warn:  return "[WARN ]";
            case Level::Error: return "[ERROR]";
        }
        return "[?????]";
    }

    template <typename... Args>
    void log(Level lvl, std::format_string<Args...> fmt, Args&&... args) {
        if (lvl < g_level.load(std::memory_order_relaxed)) return;
        std::println("{} {}", prefix(lvl), std::format(fmt, std::forward<Args>(args)...));
    }

} // namespace logging

#define LOG_DEBUG(...) ::logging::log(::logging::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ::logging::log(::logging::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::logging::log(::logging::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::logging::log(::logging::Level::Error, __VA_ARGS__)
