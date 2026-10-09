#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "memory/guest_memory.hpp"

namespace kura::loader {

// ELF64 program segment (parsed from program headers).
struct ElfSegment {
    std::uint32_t type = 0;  // p_type (PT_LOAD = 1)
    std::uint32_t flags = 0; // p_flags
    std::uint64_t offset = 0;
    std::uint64_t vaddr = 0;
    std::uint64_t filesz = 0;
    std::uint64_t memsz = 0;
    std::uint64_t align = 0;
};

struct ElfImage {
    std::uint64_t entry = 0;
    std::vector<ElfSegment> segments;
};

// Pure parser — validates magic/class/endian/machine and bounds-checks
// every field. Returns nullopt for anything that isn't a well-formed
// little-endian x86-64 ELF64.
std::optional<ElfImage> parse_elf64(const std::byte* data, std::size_t size);

// Maps PT_LOAD segments into guest memory and copies file contents.
// (M2 scope: non-overlapping segments; real-world overlapping PT_LOADs
// get page merging later.)
bool load_into(const ElfImage& image, const std::byte* data, std::size_t size,
               GuestMemory& memory);

} // namespace kura::loader
