#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kura {

// Sparse guest address space (design doc §5.5). Regions are host-side
// vectors — process-local and bounded by what's mapped, per the host
// safety guarantees (§11). Cross-region accesses are rejected.
class GuestMemory {
public:
    struct Region {
        std::uint64_t base = 0;
        std::uint64_t size = 0;
        std::vector<std::byte> data;
    };

    // Maps a zero-filled region. Fails on overlap or overflow.
    bool map(std::uint64_t base, std::uint64_t size);
    bool unmap(std::uint64_t base, std::uint64_t size);

    bool is_mapped(std::uint64_t addr, std::uint64_t len) const;

    // Host pointer if [addr, addr+len) lies entirely inside ONE region.
    std::byte* host_ptr(std::uint64_t addr, std::uint64_t len);
    const std::byte* host_ptr(std::uint64_t addr, std::uint64_t len) const;

    bool read(std::uint64_t addr, void* out, std::uint64_t len) const;
    bool write(std::uint64_t addr, const void* in, std::uint64_t len);

    template <typename T>
    bool read_value(std::uint64_t addr, T& out) const {
        return read(addr, &out, sizeof(T));
    }

    template <typename T>
    bool write_value(std::uint64_t addr, const T& value) {
        return write(addr, &value, sizeof(T));
    }

    const std::vector<Region>& regions() const { return regions_; }

private:
    std::vector<Region> regions_;
};

} // namespace kura
