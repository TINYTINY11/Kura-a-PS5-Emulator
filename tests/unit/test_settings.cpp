#include "common/settings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

using kura::settings::Settings;
using kura::settings::load;
using kura::settings::save;

void test_missing_file_leaves_defaults() {
    Settings s;
    s.firmware_path = "untouched"; // must survive a failed load
    s.log_level = "warn";
    CHECK(!load("definitely_missing_kura_settings.cfg", s));
    CHECK(s.firmware_path == "untouched");
    CHECK(s.log_level == "warn");
}

void test_round_trip() {
    namespace fs = std::filesystem;
    const auto p = (fs::temp_directory_path() / "kura_settings_rt.cfg").string();

    Settings s;
    s.firmware_path = "C:\\Games\\PS5UPDATE.PUP";
    s.log_level = "debug";
    s.first_run_done = true;
    CHECK(save(p, s));

    Settings r;
    CHECK(load(p, r));
    CHECK(r.firmware_path == s.firmware_path);
    CHECK(r.log_level == "debug");
    CHECK(r.first_run_done);
    fs::remove(p);
}

void test_defaults_when_fresh() {
    Settings r;
    CHECK(load((std::filesystem::temp_directory_path() / "kura_settings_none.cfg")
                   .string(),
               r) == false);
}

void test_malformed_and_unknown_lines_ignored() {
    namespace fs = std::filesystem;
    const auto p = fs::temp_directory_path() / "kura_settings_bad.cfg";
    {
        std::ofstream o(p);
        o << "# comment line\n"
             "\n"
             "firmware_path=/x/y.pUP\n"
             "this line has no equals sign\n"
             "log_level=warn\n"
             "unknown_future_key=42\n"
             "first_run_done=true\n";
    }
    Settings r;
    CHECK(load(p.string(), r));
    CHECK(r.firmware_path == "/x/y.pUP");
    CHECK(r.log_level == "warn");
    CHECK(r.first_run_done);
    fs::remove(p);
}

} // namespace

int main() {
    test_missing_file_leaves_defaults();
    test_round_trip();
    test_defaults_when_fresh();
    test_malformed_and_unknown_lines_ignored();
    if (g_failures == 0) std::cout << "test_settings: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
