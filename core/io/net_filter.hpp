#pragma once

#include <string_view>

// PSN safety filter (M0).
//
// Kura and any software running inside it must never contact PlayStation
// Network / Sony online services. The emulator implements no real networking
// itself (design doc §5.10 — sceNet/sceHttp are stubbed), and every outbound
// host check passes through this filter first, so if networking is ever
// implemented it stays deny-by-default for PSN endpoints.

namespace kura::net {

// Returns true if `host` (optionally "host:port") is a Sony/PSN endpoint
// that must be refused. Matching is case-insensitive and matches whole
// domain suffixes: "cta2.playstation.net" is blocked, "notplaystation.net"
// is not.
bool is_blocked_host(std::string_view host);

} // namespace kura::net
