#include "pup.hpp"

#include <array>
#include <cstring>
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

// Builds a synthetic SLB2 header — completely made-up values, no real
// firmware bytes (repo must never contain Sony data).
std::array<std::byte, kura::pup::kHeaderSize> make_synthetic_header() {
    std::array<std::byte, kura::pup::kHeaderSize> h{};
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) h[off + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    };
    auto put64 = [&](std::size_t off, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) h[off + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    };

    h[0] = std::byte{'S'};
    h[1] = std::byte{'L'};
    h[2] = std::byte{'B'};
    h[3] = std::byte{'2'};
    put32(0x04, 3);          // version
    put32(0x08, 0x10000);    // block size
    put32(0x0C, 1);
    put64(0x10, 0x123456);   // field_10
    put64(0x18, 0);
    put32(0x20, 2);
    put32(0x24, 4096);       // total size (fake)
    const char* name = "TESTPUP1.PUP";
    for (std::size_t i = 0; i < 16 && name[i]; ++i) h[0x30 + i] = static_cast<std::byte>(name[i]);
    return h;
}

void test_parse_valid() {
    auto h = make_synthetic_header();
    auto parsed = kura::pup::parse_header(h.data(), h.size());
    CHECK(parsed.has_value());
    CHECK(parsed->version == 3);
    CHECK(parsed->block_size == 0x10000);
    CHECK(parsed->field_0c == 1);
    CHECK(parsed->field_10 == 0x123456);
    CHECK(parsed->field_18 == 0);
    CHECK(parsed->field_20 == 2);
    CHECK(parsed->total_size == 4096);
    CHECK(parsed->name == "TESTPUP1.PUP");
}

void test_rejects_bad_magic() {
    auto h = make_synthetic_header();
    h[0] = std::byte{'X'};
    CHECK(!kura::pup::parse_header(h.data(), h.size()).has_value());

    auto h2 = make_synthetic_header();
    CHECK(!kura::pup::parse_header(h2.data(), 8)); // too small
    CHECK(!kura::pup::is_slb2_magic(h2.data(), 2)); // tiny buffer
}

void test_validate() {
    auto h = make_synthetic_header();
    auto parsed = kura::pup::parse_header(h.data(), h.size());
    CHECK(parsed.has_value());

    auto ok = kura::pup::validate(*parsed, 4096);
    CHECK(ok.size_matches);
    auto bad = kura::pup::validate(*parsed, 9999);
    CHECK(!bad.size_matches);
    CHECK(!bad.note.empty());
}

void test_entropy() {
    // Low entropy: repeated byte
    std::array<std::uint8_t, 4096> flat{};
    flat.fill(0x41);
    CHECK(kura::pup::shannon_entropy(flat.data(), flat.size()) < 0.01);

    // Full-range cycling: exactly 8 bits/byte
    std::array<std::uint8_t, 4096> uniform{};
    for (std::size_t i = 0; i < uniform.size(); ++i) uniform[i] = static_cast<std::uint8_t>(i & 0xFF);
    CHECK(kura::pup::shannon_entropy(uniform.data(), uniform.size()) == 8.0);

    CHECK(kura::pup::classify_entropy(7.9).find("encrypted") != std::string_view::npos);
    CHECK(kura::pup::classify_entropy(2.0).find("plaintext") != std::string_view::npos);
}

} // namespace

int main() {
    test_parse_valid();
    test_rejects_bad_magic();
    test_validate();
    test_entropy();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "[kura_pup_tests] all checks passed\n";
    return 0;
}
