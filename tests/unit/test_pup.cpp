#include "pup.hpp"

#include <array>
#include <cstring>
#include <iostream>
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

// ---- modern entry table (synthetic — mirrors community-documented layout) ----

// Builds a synthetic decrypted-style PUP prefix: header with entry count at
// 0x0C, then N 0x30-byte entries at 0x30, then a fake payload area so that
// each entry's [offset, offset+size) lands inside the buffer.
std::vector<std::byte> make_synthetic_table(std::uint32_t count,
                                            std::vector<std::uint64_t> ids,
                                            std::vector<std::uint32_t> flags) {
    const std::size_t payload_off = 0x1000;
    const std::size_t total = payload_off + count * 0x100;
    std::vector<std::byte> buf(total, std::byte{0});

    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            buf[off + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    };
    auto put64 = [&](std::size_t off, std::uint64_t v) {
        for (int i = 0; i < 8; ++i)
            buf[off + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    };

    buf[0] = std::byte{'S'}; buf[1] = std::byte{'L'};
    buf[2] = std::byte{'B'}; buf[3] = std::byte{'2'};
    put32(0x0C, count);

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t e = 0x30 + static_cast<std::size_t>(i) * 0x30;
        put64(e + 0x00, ids[i]);
        put64(e + 0x08, 0x100);                    // compressed size
        put64(e + 0x10, 0x100);                    // uncompressed size
        put64(e + 0x18, payload_off + i * 0x100);  // offset
        put32(e + 0x20, flags[i]);
    }
    return buf;
}

void test_entry_table_parses() {
    auto buf = make_synthetic_table(2, {0x100, 0x20000},
                                    {kura::pup::kEntryFlagEncrypted,
                                     kura::pup::kEntryFlagCompressed});
    auto entries = kura::pup::parse_entry_table(buf.data(), buf.size());
    CHECK(entries.has_value());
    if (!entries) return;
    CHECK(entries->size() == 2);
    CHECK((*entries)[0].id == 0x100);
    CHECK(kura::pup::entry_name(0x100) == "eap_kernel");
    CHECK((*entries)[0].encrypted());
    CHECK(!(*entries)[0].compressed());
    CHECK((*entries)[1].id == 0x20000);
    CHECK(kura::pup::entry_name(0x20000) == "gpu_ucode");
    CHECK((*entries)[1].compressed());
    CHECK((*entries)[0].offset == 0x1000);
    CHECK((*entries)[1].offset == 0x1100);
}

void test_entry_table_rejects_garbage() {
    // Encrypted container: entry count at 0x0C is ciphertext — huge count
    // must be rejected as "not readable", not crash.
    auto enc = make_synthetic_table(1, {0x100}, {0});
    for (int i = 0x0C; i < 0x10; ++i) enc[static_cast<std::size_t>(i)] = std::byte{0xFF};
    CHECK(!kura::pup::parse_entry_table(enc.data(), enc.size()).has_value());

    // Entry pointing outside the file image → reject.
    auto oob = make_synthetic_table(1, {0x100}, {0});
    for (int i = 0x18; i < 0x20; ++i) oob[0x30 + static_cast<std::size_t>(i)] = std::byte{0xFF};
    CHECK(!kura::pup::parse_entry_table(oob.data(), oob.size()).has_value());

    // Unknown-but-plausible IDs get empty names, not an error.
    CHECK(kura::pup::entry_name(0xDEAD).empty());

    // Bad magic → no table.
    auto bad = make_synthetic_table(1, {0x100}, {0});
    bad[0] = std::byte{'X'};
    CHECK(!kura::pup::parse_entry_table(bad.data(), bad.size()).has_value());
}

} // namespace

int main() {
    test_parse_valid();
    test_rejects_bad_magic();
    test_validate();
    test_entropy();
    test_entry_table_parses();
    test_entry_table_rejects_garbage();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "[kura_pup_tests] all checks passed\n";
    return 0;
}
