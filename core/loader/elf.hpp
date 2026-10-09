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
// little-endian x86-64 ELF64 (accepts the SCE e_type extension range
// 0xFE00-0xFE1F found in SELF containers).
std::optional<ElfImage> parse_elf64(const std::byte* data, std::size_t size);

// SELF/encrypted-component scanning: community RE shows the inner ELF
// headers of PS5 components are plaintext inside otherwise-encrypted
// blobs (ELF found at arbitrary offsets like 0x8D318). Scans for a valid
// ELF64 starting at `start` and returns its offset + parsed image.
struct EmbeddedElf {
    std::size_t offset = 0;
    ElfImage image;
};
std::optional<EmbeddedElf> find_embedded_elf(const std::byte* data, std::size_t size,
                                             std::size_t start = 0);

// Maps PT_LOAD segments into guest memory and copies file contents.
// (M2 scope: non-overlapping segments; real-world overlapping PT_LOADs
// get page merging later.)
bool load_into(const ElfImage& image, const std::byte* data, std::size_t size,
               GuestMemory& memory);

} // namespace kura::loader
