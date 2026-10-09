#include "common/log.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

void test_level_names() {
    using kura::log::Level;
    Level l{};
    CHECK(kura::log::parse_level("info", l) && l == Level::Info);
    CHECK(kura::log::parse_level("WARN", l) && l == Level::Warn);
    CHECK(kura::log::parse_level("warning", l) && l == Level::Warn);
    CHECK(kura::log::parse_level("trace", l) && l == Level::Trace);
    CHECK(kura::log::parse_level("off", l) && l == Level::Off);
    CHECK(!kura::log::parse_level("bogus", l));
    CHECK(kura::log::level_name(Level::Error) == "ERROR");
    CHECK(kura::log::level_name(Level::Trace) == "TRACE");
}

void test_filtering() {
    using kura::log::Level;
    std::vector<kura::log::Entry> captured;
    kura::log::set_sink([&](const kura::log::Entry& e) { captured.push_back(e); });

    kura::log::set_min_level(Level::Warn);
    kura::log::info("test", "this should be filtered out");
    CHECK(captured.empty());

    kura::log::warn("test", "visible warning");
    CHECK(captured.size() == 1);
    CHECK(captured[0].channel == "test");
    CHECK(captured[0].message == "visible warning");
    CHECK(captured[0].level == Level::Warn);

    kura::log::set_min_level(Level::Off);
    kura::log::error("test", "even errors are off");
    CHECK(captured.size() == 1);

    kura::log::set_min_level(Level::Trace);
    kura::log::reset_sink();
}

void test_stream_format() {
    using kura::log::Level;
    std::vector<kura::log::Entry> captured;
    kura::log::set_sink([&](const kura::log::Entry& e) { captured.push_back(e); });
    kura::log::set_min_level(Level::Trace);

    kura::log::info("loader", "mapped ", 3, " segments at 0x", std::hex, 0x400000);
    CHECK(captured.size() == 1);
    CHECK(captured[0].message == "mapped 3 segments at 0x400000");

    KURA_DEBUG("gpu.cmd", "packet ", std::hex, 0x3f, " handled");
    CHECK(captured.size() == 2);
    CHECK(captured[1].channel == "gpu.cmd");
    CHECK(captured[1].message == "packet 3f handled");

    kura::log::reset_sink();
}

void test_file_sink() {
    using kura::log::Level;
    const auto path = std::filesystem::temp_directory_path() / "kura_log_sink_test.txt";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    kura::log::set_min_level(Level::Trace);
    CHECK(kura::log::set_file_sink(path.string()));
    kura::log::info("filesink", "hello from the file sink");
    kura::log::close_file_sink();

    std::ifstream in(path);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("[filesink]") != std::string::npos);
    CHECK(all.find("hello from the file sink") != std::string::npos);

    std::filesystem::remove(path, ec);
}

} // namespace

int main() {
    test_level_names();
    test_filtering();
    test_stream_format();
    test_file_sink();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "[kura_tests] all checks passed\n";
    return 0;
}
