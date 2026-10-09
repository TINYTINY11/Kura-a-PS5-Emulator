#include "common/log.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "io/net_filter.hpp"

#ifndef KURA_VERSION_STRING
#define KURA_VERSION_STRING "0.1.0-dev"
#endif

namespace {

void print_usage() {
    std::cout <<
        "Kura — PS5 emulator  (M0 foundations)\n"
        "\n"
        "Usage: kura [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help              Show this help and exit\n"
        "  -v, --version           Show version and exit\n"
        "  --log-level <level>     trace|debug|info|warn|error|off  (default: info)\n"
        "  --log-file <path>       Also write logs to a file\n"
        "  --firmware <path>       Run the firmware pipeline against a PS5UPDATE.PUP\n"
        "  --check-host <host>     Check a host against the PSN block list\n";
}

// True when the console was created *for* us (double-click / Explorer) —
// i.e. our process is the only one attached, so the window would flash
// closed on exit. In that case we pause so the output stays visible.
bool launched_by_double_click() {
#ifdef _WIN32
    DWORD pids[2] = {0, 0};
    return GetConsoleProcessList(pids, 2) == 1;
#else
    return false;
#endif
}

int run(int argc, char** argv) {
    using namespace kura;

    std::string firmware_path;
    std::string check_host;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "error: " << flag << " needs a value\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") { print_usage(); return 0; }
        if (arg == "-v" || arg == "--version") {
            std::cout << "Kura " << KURA_VERSION_STRING << " (M0 foundations)\n";
            return 0;
        }
        if (arg == "--log-level") {
            const char* v = need_value("--log-level");
            log::Level lvl{};
            if (!log::parse_level(v, lvl)) {
                std::cerr << "error: unknown log level '" << v << "'\n";
                return 1;
            }
            log::set_min_level(lvl);
        } else if (arg == "--log-file") {
            const char* v = need_value("--log-file");
            if (!log::set_file_sink(v)) {
                std::cerr << "error: cannot open log file '" << v << "'\n";
                return 1;
            }
        } else if (arg == "--firmware") {
            firmware_path = need_value("--firmware");
        } else if (arg == "--check-host") {
            check_host = need_value("--check-host");
        } else {
            std::cerr << "error: unknown option '" << arg << "' (try --help)\n";
            return 1;
        }
    }

    // PSN safety check: the emulator and emulated software never contact PSN.
    if (!check_host.empty()) {
        const bool blocked = net::is_blocked_host(check_host);
        std::cout << check_host << ": " << (blocked ? "BLOCKED (PSN/Sony)" : "allowed") << "\n";
        return 0;
    }

    log::info("boot", "Kura ", KURA_VERSION_STRING, " starting (M0 foundations)");
    log::debug("boot", "log system ready — channels: boot, loader, kernel.sys, gpu.cmd, net");

    if (!firmware_path.empty()) {
        // ---- First boot sequence (design doc §5.2) ----
        namespace fs = std::filesystem;

        log::info("boot", "stage 0: firmware file located — ", firmware_path);
        std::error_code ec;
        if (!fs::exists(firmware_path, ec)) {
            log::error("boot", "stage 0 FAILED — file not found");
            return 1;
        }
        log::info("boot", "stage 0 OK");

        // Locate kura_pup next to kura.exe (the pipeline tool stays a
        // separate binary — design rule: never linked into the emulator).
        fs::path tool = fs::path(argv[0]).parent_path() / "kura_pup.exe";
        if (!fs::exists(tool, ec)) tool = fs::path("kura_pup.exe");
        log::info("boot", "stage 1: SLB2 header parse via ", tool.filename().string());

        // cmd.exe strips the first+last quote when /c's argument starts with
        // one — wrap the whole command so paths with spaces survive.
        const std::string inner = "\"" + tool.string() + "\" \"" + firmware_path + "\"";
        const std::string cmd = "\"" + inner + "\"";
        const int rc = std::system(cmd.c_str());

        if (rc == 0) {
            log::info("boot", "stage 1 OK — payload extractable");
            log::info("boot", "stage 2: READY — extraction (stage 3) can proceed");
            return 0;
        }
        if (rc == 3) {
            log::error("boot", "stage 1 OK — but BOOT HALTED at stage 2: payload is encrypted");
            log::error("boot", "keys required to decrypt — see docs/RE-pup.md and docs/DESIGN.md §5.2/§12");
            log::info("boot", "after decryption: M2 loader+CPU → M3 kernel → M4 GPU → M5 services → M6 shell");
            return 4;
        }
        log::error("boot", "firmware pipeline failed with code ", rc);
        return 4;
    }

    log::info("boot", "no firmware installed yet — the first-run wizard arrives in M1");
    log::info("boot", "run with --help for usage");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const bool pause_on_exit = launched_by_double_click();
    const int rc = run(argc, argv);
    if (pause_on_exit) {
        std::cout << "\nPress Enter to exit..." << std::endl;
        (void)std::cin.get();
    }
    return rc;
}
