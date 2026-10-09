#pragma once

#include <string>

// Per-user settings (M3 stage 1.5 — the first-run boot experience).
//
// Deliberately tiny: key=value lines, no dependencies. The wizard in the
// boot GUI writes these; the CLI and future frontends read them. Paths are
// host-side only — nothing here ever reaches the guest (design doc §11).
namespace kura::settings {

struct Settings {
    std::string firmware_path;         // remembered PS5UPDATE.PUP location
    std::string log_level = "info";    // trace|debug|info|warn|error|off
    bool first_run_done = false;       // wizard completed at least once
};

// Per-user config location: %APPDATA%\Kura\kura.cfg on Windows,
// ~/.config/kura/kura.cfg elsewhere. Never inside the repo — settings are
// user data, not project data.
std::string default_path();

// Loading a missing or malformed file leaves `out` at defaults and returns
// false; a loaded file returns true (unrecognized keys are preserved in the
// file on the next save only if we round-trip them — stage 1 drops them).
bool load(const std::string& path, Settings& out);
bool save(const std::string& path, const Settings& settings);

} // namespace kura::settings
