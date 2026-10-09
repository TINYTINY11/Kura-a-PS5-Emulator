#include "loader/elf.hpp"

#include <cstring>
#include <utility>

namespace kura::loader {
namespace {

std::uint16_t rd16(const std::byte* p) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) |
           (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1])) << 8);
}

std::uint32_t rd32(const std::byte* p) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24);
}

std::uint64_t rd64(const std::byte* p) {
    return static_cast<std::uint64_t>(rd32(p)) |
           (static_cast<std::uint64_t>(rd32(p + 4)) << 32);
}

bool in_range(std::size_t size, std::uint64_t off, std::uint64_t len) {
    if (off > size) return false;
    return len <= size - off;
}

constexpr std::size_t kEhdrSize = 64;
constexpr std::size_t kPhdrSize = 56;
constexpr std::uint8_t kClass64 = 2;
constexpr std::uint8_t kDataLE = 1;
constexpr std::uint16_t kMachineX86_64 = 0x3E;
constexpr std::uint32_t kPtLoad = 1;

} // namespace

std::optional<ElfImage> parse_elf64(const std::byte* data, std::size_t size) {
    if (size < kEhdrSize) return std::nullopt;

    auto b = [&](std::size_t i) { return std::to_integer<std::uint8_t>(data[i]); };
    if (b(0) != 0x7F || b(1) != 'E' || b(2) != 'L' || b(3) != 'F') return std::nullopt;
    if (b(4) != kClass64) return std::nullopt;  // 64-bit
    if (b(5) != kDataLE) return std::nullopt;   // little-endian
    if (b(6) != 1) return std::nullopt;         // ELF version

    const std::uint16_t type = rd16(data + 16);
    const std::uint16_t machine = rd16(data + 18);
    // ET_EXEC / ET_DYN, plus the Sony SCE extension range used inside
    // SELF containers (ET_SCE_EXEC 0xFE00, ET_SCE_REPLAY_EXEC 0xFE04,
    // ET_SCE_DYNEXEC 0xFE10, ET_SCE_DYNAMIC 0xFE18, ...): community RE
    // confirms these headers sit in plaintext inside decrypted components.
    const bool sce_type = type >= 0xFE00 && type <= 0xFE1F;
    if (type != 2 && type != 3 && !sce_type) return std::nullopt;
    if (machine != kMachineX86_64) return std::nullopt;

    ElfImage img;
    img.entry = rd64(data + 24);
    const std::uint64_t phoff = rd64(data + 32);
    const std::uint16_t phentsize = rd16(data + 54);
    const std::uint16_t phnum = rd16(data + 56);

    if (phnum == 0) return img; // no segments is legal (nothing to load)
    if (phentsize < kPhdrSize) return std::nullopt;

    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::uint64_t off = phoff + static_cast<std::uint64_t>(i) * phentsize;
        if (!in_range(size, off, kPhdrSize)) return std::nullopt;

        const std::byte* ph = data + off;
        ElfSegment s;
        s.type = rd32(ph + 0);
        s.flags = rd32(ph + 4);
        s.offset = rd64(ph + 8);
        s.vaddr = rd64(ph + 16);
        s.filesz = rd64(ph + 32);
        s.memsz = rd64(ph + 40);
        s.align = rd64(ph + 48);

        if (s.filesz > s.memsz) return std::nullopt; // can't fit
        if (s.type == kPtLoad && !in_range(size, s.offset, s.filesz)) return std::nullopt;

        img.segments.push_back(s);
    }
    return img;
}

std::optional<EmbeddedElf> find_embedded_elf(const std::byte* data, std::size_t size,
                                             std::size_t start) {
    if (size < kEhdrSize) return std::nullopt;
    for (std::size_t off = start; off + kEhdrSize <= size; ++off) {
        const auto b = [&](std::size_t i) {
            return std::to_integer<std::uint8_t>(data[off + i]);
        };
        if (b(0) != 0x7F || b(1) != 'E' || b(2) != 'L' || b(3) != 'F') continue;
        if (auto img = parse_elf64(data + off, size - off)) {
            return EmbeddedElf{off, std::move(*img)};
        }
        // fake/other-architecture magic — keep scanning
    }
    return std::nullopt;
}

bool load_into(const ElfImage& image, const std::byte* data, std::size_t size,
               GuestMemory& memory) {
    for (const auto& s : image.segments) {
        if (s.type != kPtLoad) continue;
        if (s.memsz == 0) continue;
        if (s.vaddr + s.memsz < s.vaddr) return false; // overflow
        if (!in_range(size, s.offset, s.filesz)) return false;

        if (!memory.map(s.vaddr, s.memsz)) return false; // overlap / bad addr
        if (s.filesz > 0) {
            if (!memory.write(s.vaddr, data + s.offset, s.filesz)) return false;
        }
    }
    return true;
}

} // namespace kura::loader
