#include "memory/guest_memory.hpp"

#include <cstring>

namespace kura {
namespace {

// addr+len inside [base, base+size), overflow-safe.
bool contains(std::uint64_t base, std::uint64_t size, std::uint64_t addr, std::uint64_t len) {
    if (len == 0) return addr >= base && addr <= base + size;
    if (addr < base) return false;
    const std::uint64_t off = addr - base;
    if (off > size) return false;
    return len <= size - off;
}

} // namespace

bool GuestMemory::map(std::uint64_t base, std::uint64_t size) {
    if (size == 0) return false;
    if (base + size < base) return false; // overflow
    for (const auto& r : regions_) {
        // overlap test: [base, base+size) vs [r.base, r.base+r.size)
        if (base < r.base + r.size && r.base < base + size) return false;
    }
    Region r;
    r.base = base;
    r.size = size;
    r.data.resize(static_cast<std::size_t>(size));
    regions_.push_back(std::move(r));
    return true;
}

bool GuestMemory::unmap(std::uint64_t base, std::uint64_t size) {
    for (auto it = regions_.begin(); it != regions_.end(); ++it) {
        if (it->base == base && it->size == size) {
            regions_.erase(it);
            return true;
        }
    }
    return false;
}

bool GuestMemory::is_mapped(std::uint64_t addr, std::uint64_t len) const {
    for (const auto& r : regions_) {
        if (contains(r.base, r.size, addr, len)) return true;
    }
    return false;
}

std::byte* GuestMemory::host_ptr(std::uint64_t addr, std::uint64_t len) {
    for (auto& r : regions_) {
        if (contains(r.base, r.size, addr, len)) {
            return r.data.data() + (addr - r.base);
        }
    }
    return nullptr;
}

const std::byte* GuestMemory::host_ptr(std::uint64_t addr, std::uint64_t len) const {
    for (const auto& r : regions_) {
        if (contains(r.base, r.size, addr, len)) {
            return r.data.data() + (addr - r.base);
        }
    }
    return nullptr;
}

bool GuestMemory::read(std::uint64_t addr, void* out, std::uint64_t len) const {
    const std::byte* p = host_ptr(addr, len);
    if (p == nullptr) return false;
    std::memcpy(out, p, static_cast<std::size_t>(len));
    return true;
}

bool GuestMemory::write(std::uint64_t addr, const void* in, std::uint64_t len) {
    std::byte* p = host_ptr(addr, len);
    if (p == nullptr) return false;
    std::memcpy(p, in, static_cast<std::size_t>(len));
    return true;
}

} // namespace kura
