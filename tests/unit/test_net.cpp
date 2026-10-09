#include "io/net_filter.hpp"

#include <iostream>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

} // namespace

int main() {
    using kura::net::is_blocked_host;

    // PSN / Sony endpoints — must be blocked
    CHECK(is_blocked_host("playstation.net"));
    CHECK(is_blocked_host("en.account.playstation.net"));
    CHECK(is_blocked_host("cta2.playstation.net:443"));
    CHECK(is_blocked_host("PLAYSTATION.NET"));          // case-insensitive
    CHECK(is_blocked_host("playstation.net."));         // trailing dot
    CHECK(is_blocked_host("store.playstation.com"));
    CHECK(is_blocked_host("www.playstation.com"));
    CHECK(is_blocked_host("my.account.sonyentertainmentnetwork.com"));
    CHECK(is_blocked_host("psn.playstationnetwork.com"));

    // Look-alikes — must NOT be blocked (whole-label matching)
    CHECK(!is_blocked_host("notplaystation.net"));
    CHECK(!is_blocked_host("playstation.net.evil.example"));
    CHECK(!is_blocked_host("example.com"));
    CHECK(!is_blocked_host(""));
    CHECK(!is_blocked_host("localhost"));

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "[kura_net_tests] all checks passed\n";
    return 0;
}
