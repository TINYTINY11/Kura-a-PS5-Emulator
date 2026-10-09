#include "common/settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace kura::settings {
namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

} // namespace

std::string default_path() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    fs::path base = (appdata != nullptr && *appdata != '\0')
                        ? fs::path(appdata)
                        : fs::temp_directory_path();
    return (base / "Kura" / "kura.cfg").string();
#else
    const char* home = std::getenv("HOME");
    fs::path base = (home != nullptr && *home != '\0') ? fs::path(home)
                                                       : fs::temp_directory_path();
    return (base / ".config" / "kura" / "kura.cfg").string();
#endif
}

bool load(const std::string& path, Settings& out) {
    std::ifstream in(path);
    if (!in) return false;

    Settings s; // parse into fresh defaults
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue; // malformed — ignore, don't die
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "firmware_path") s.firmware_path = value;
        else if (key == "log_level") s.log_level = value;
        else if (key == "first_run_done") s.first_run_done = (value == "1" || value == "true");
        // unknown keys are ignored (forward compatibility)
    }
    out = s;
    return true;
}

bool save(const std::string& path, const Settings& settings) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "# Kura settings — managed by the first-run wizard; edit by hand at your own risk\n"
        << "firmware_path=" << settings.firmware_path << "\n"
        << "log_level=" << settings.log_level << "\n"
        << "first_run_done=" << (settings.first_run_done ? "1" : "0") << "\n";
    return static_cast<bool>(out);
}

} // namespace kura::settings
