#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// Kura logging / trace system (M0).
//
// Everything in Kura logs through named channels ("boot", "loader",
// "kernel.sys", "gpu.cmd", ...) with a severity level. Output goes to the
// console by default, optionally also to a file, and can be redirected to a
// custom sink (used by tests and, later, the debug overlay).

namespace kura::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

struct Entry {
    Level level = Level::Info;
    std::string channel;
    std::string message;
};

using Sink = std::function<void(const Entry&)>;

// --- severity filtering -----------------------------------------------------

void set_min_level(Level level);
Level min_level();
bool enabled(Level level);

// "trace" | "debug" | "info" | "warn" | "error" | "off" (case-insensitive)
bool parse_level(std::string_view name, Level& out);
std::string_view level_name(Level level);

// --- output sinks -----------------------------------------------------------

// Replaces the console sink. The file sink (if open) still receives entries.
void set_sink(Sink sink);
void reset_sink();

// Also write every entry to this file (append). Returns false on failure.
bool set_file_sink(std::string_view path);
void close_file_sink();

// --- writing ----------------------------------------------------------------

void write(Level level, std::string_view channel, std::string_view message);

// Stream-style concatenation:
//   kura::log::info("loader", "mapped ", 3, " segments at 0x", std::hex, addr);
template <typename... Args>
void writef(Level level, std::string_view channel, Args&&... args) {
    if (!enabled(level)) return;
    std::ostringstream os;
    (os << ... << std::forward<Args>(args));
    write(level, channel, os.str());
}

template <typename... Args> void trace(std::string_view ch, Args&&... a) { writef(Level::Trace, ch, std::forward<Args>(a)...); }
template <typename... Args> void debug(std::string_view ch, Args&&... a) { writef(Level::Debug, ch, std::forward<Args>(a)...); }
template <typename... Args> void info (std::string_view ch, Args&&... a) { writef(Level::Info,  ch, std::forward<Args>(a)...); }
template <typename... Args> void warn (std::string_view ch, Args&&... a) { writef(Level::Warn,  ch, std::forward<Args>(a)...); }
template <typename... Args> void error(std::string_view ch, Args&&... a) { writef(Level::Error, ch, std::forward<Args>(a)...); }

} // namespace kura::log

// Macro forms — convenient for hot paths (channel is just the first argument):
#define KURA_LOG(level, ...) ::kura::log::writef(level, __VA_ARGS__)
#define KURA_TRACE(...) ::kura::log::trace(__VA_ARGS__)
#define KURA_DEBUG(...) ::kura::log::debug(__VA_ARGS__)
#define KURA_INFO(...)  ::kura::log::info(__VA_ARGS__)
#define KURA_WARN(...)  ::kura::log::warn(__VA_ARGS__)
#define KURA_ERROR(...) ::kura::log::error(__VA_ARGS__)
