#include "common/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace kura::log {
namespace {

std::mutex g_mutex;
std::atomic<Level> g_min_level{Level::Info};
Sink g_sink;       // custom sink; empty = default console sink
std::ofstream g_file;
bool g_console_vt_ready = false;

void ensure_console_vt() {
#ifdef _WIN32
    if (g_console_vt_ready) return;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    g_console_vt_ready = true;
#endif
}

std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

const char* color_for(Level level) {
    switch (level) {
    case Level::Trace: return "\x1b[90m";
    case Level::Debug: return "\x1b[36m";
    case Level::Info:  return "\x1b[32m";
    case Level::Warn:  return "\x1b[33m";
    case Level::Error: return "\x1b[31m";
    default:           return "\x1b[0m";
    }
}

std::string format_plain(const Entry& e) {
    std::string tag(level_name(e.level));
    // pad to 5 chars so columns line up: TRACE DEBUG  INFO  WARN ERROR
    while (tag.size() < 5) tag.push_back(' ');
    return timestamp() + " [" + tag + "] [" + e.channel + "] " + e.message;
}

void console_sink(const Entry& e) {
    ensure_console_vt();
    const std::string line = format_plain(e);
    const bool to_stderr = e.level >= Level::Warn;
    std::fprintf(to_stderr ? stderr : stdout, "%s%s\x1b[0m\n", color_for(e.level), line.c_str());
}

} // namespace

void set_min_level(Level level) { g_min_level.store(level); }
Level min_level() { return g_min_level.load(); }

bool enabled(Level level) {
    const Level min = g_min_level.load();
    return min != Level::Off && level >= min;
}

bool parse_level(std::string_view name, Level& out) {
    std::string s(name);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "trace") { out = Level::Trace; return true; }
    if (s == "debug") { out = Level::Debug; return true; }
    if (s == "info")  { out = Level::Info;  return true; }
    if (s == "warn" || s == "warning") { out = Level::Warn; return true; }
    if (s == "error") { out = Level::Error; return true; }
    if (s == "off" || s == "none") { out = Level::Off; return true; }
    return false;
}

std::string_view level_name(Level level) {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO";
    case Level::Warn:  return "WARN";
    case Level::Error: return "ERROR";
    case Level::Off:   return "OFF";
    }
    return "?";
}

void set_sink(Sink sink) {
    std::lock_guard lock(g_mutex);
    g_sink = std::move(sink);
}

void reset_sink() {
    std::lock_guard lock(g_mutex);
    g_sink = nullptr;
}

bool set_file_sink(std::string_view path) {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    g_file.open(std::string(path), std::ios::app);
    return g_file.is_open();
}

void close_file_sink() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) g_file.close();
}

void write(Level level, std::string_view channel, std::string_view message) {
    if (!enabled(level)) return;
    Entry e{level, std::string(channel), std::string(message)};
    std::lock_guard lock(g_mutex);
    if (g_sink) {
        g_sink(e);
    } else {
        console_sink(e);
    }
    if (g_file.is_open()) {
        g_file << format_plain(e) << '\n';
        g_file.flush(); // keep logs intact if we crash during boot debugging
    }
}

} // namespace kura::log
