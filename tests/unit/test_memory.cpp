#include "memory/guest_memory.hpp"

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

void test_map_read_write() {
    kura::GuestMemory mem;
    CHECK(mem.map(0x400000, 0x1000));
    CHECK(mem.is_mapped(0x400000, 0x1000));
    CHECK(!mem.is_mapped(0x400000 + 0x1000, 1)); // one past the end

    const std::uint64_t value = 0x1122334455667788ull;
    CHECK(mem.write_value(0x400010, value));
    std::uint64_t readback = 0;
    CHECK(mem.read_value(0x400010, readback));
    CHECK(readback == value);

    // zero-filled on map
    std::uint64_t z = 0;
    CHECK(mem.read_value(0x400000, z));
    CHECK(z == 0);
}

void test_bounds() {
    kura::GuestMemory mem;
    CHECK(mem.map(0x400000, 0x1000));
    std::uint64_t v = 0;
    CHECK(!mem.read_value(0x400FFE, v));    // 8 bytes would cross the end
    CHECK(!mem.read_value(0x3FFFF8, v));    // entirely below
    CHECK(!mem.write_value(0x500000, v));   // unmapped
    std::uint8_t cross[8];
    CHECK(!mem.read(0x400FFD, cross, 8));
    CHECK(mem.read(0x400FF8, cross, 8)); // ends exactly at boundary
}

void test_overlap_and_unmap() {
    kura::GuestMemory mem;
    CHECK(mem.map(0x1000, 0x1000));
    CHECK(!mem.map(0x1800, 0x1000)); // overlaps
    CHECK(mem.map(0x3000, 0x1000));  // adjacent is fine
    CHECK(mem.unmap(0x1000, 0x1000));
    CHECK(!mem.is_mapped(0x1000, 1));
    CHECK(mem.map(0x1000, 0x1000)); // reusable after unmap
}

void test_overflow() {
    kura::GuestMemory mem;
    CHECK(!mem.map(0xFFFFFFFFFFFFFFF0ull, 0x100)); // wraps past 2^64
    CHECK(!mem.map(0x1000, 0));                     // empty region
}

void test_cross_region() {
    kura::GuestMemory mem;
    CHECK(mem.map(0x1000, 0x100));
    CHECK(mem.map(0x1200, 0x100));
    std::uint64_t v = 0;
    // an access spanning the gap between regions must fail, not read garbage
    CHECK(!mem.read(0x1180, &v, 32));
    CHECK(mem.host_ptr(0x1100, 0x100) == nullptr);
}

} // namespace

int main() {
    test_map_read_write();
    test_bounds();
    test_overlap_and_unmap();
    test_overflow();
    test_cross_region();
    if (g_failures == 0) std::cout << "test_memory: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
