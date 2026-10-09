#include "io/net_filter.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace kura::net {
namespace {

// PlayStation Network & PlayStation online service domains.
// The list grows as networking HLE matures; matching is by domain suffix.
constexpr const char* kBlockedSuffixes[] = {
    "playstation.net",
    "playstation.com",
    "playstationnetwork.com",
    "sonyentertainmentnetwork.com",
};

bool has_suffix(const std::string& host, const char* suffix) {
    const std::string s(suffix);
    if (host.size() < s.size() + 1) return false; // need at least one char + '.'
    if (host.compare(host.size() - s.size(), s.size(), s) != 0) return false;
    return host[host.size() - s.size() - 1] == '.'; // whole-label boundary
}

} // namespace

bool is_blocked_host(std::string_view host) {
    // Strip an optional port ("host:443"). Naive cut at the first ':' —
    // IPv6 literals don't appear in PSN host checks (M0 note).
    if (const auto colon = host.find(':'); colon != std::string_view::npos) {
        host = host.substr(0, colon);
    }

    std::string h(host);
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Tolerate a trailing dot ("playstation.net.").
    if (!h.empty() && h.back() == '.') h.pop_back();
    if (h.empty()) return false;

    for (const char* suffix : kBlockedSuffixes) {
        if (h == suffix) return true;
        if (has_suffix(h, suffix)) return true;
    }
    return false;
}

} // namespace kura::net
