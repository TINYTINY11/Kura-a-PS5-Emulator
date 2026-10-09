#include "kernel/dynlib.hpp"

#include <cstring>

#include "common/log.hpp"
#include "loader/elf.hpp"

namespace kura::kernel {

namespace {

constexpr std::uint32_t kShtSymtab = 2;
constexpr std::uint32_t kShtDynsym = 11;
constexpr std::uint64_t kPtLoad = 1;
constexpr std::uint16_t kEtDyn = 3;

constexpr std::uint64_t kPageSize = 0x1000;
std::uint64_t page_down(std::uint64_t v) { return v & ~(kPageSize - 1); }
std::uint64_t page_up(std::uint64_t v) {
    return (v + kPageSize - 1) & ~(kPageSize - 1);
}

// Bounds-checked little-endian reads over the image bytes.
bool rd(const std::byte* d, std::size_t n, std::size_t off, void* out,
        std::size_t len) {
    if (off > n || len > n - off) return false;
    std::memcpy(out, d + off, len);
    return true;
}

template <typename T>
bool rd(const std::byte* d, std::size_t n, std::size_t off, T& out) {
    return rd(d, n, off, &out, sizeof(T));
}

} // namespace

std::map<std::string, std::uint64_t, std::less<>>
Dynlib::parse_symbols(const std::byte* data, std::size_t size,
                      std::uint64_t base) {
    std::map<std::string, std::uint64_t, std::less<>> out;
    if (size < 0x40) return out;

    std::uint16_t etype = 0;
    std::uint64_t shoff = 0;
    std::uint16_t shentsize = 0, shnum = 0;
    if (!rd(data, size, 0x10, etype) || !rd(data, size, 0x28, shoff) ||
        !rd(data, size, 0x3A, shentsize) || !rd(data, size, 0x3C, shnum))
        return out;
    if (shentsize < 0x40 || shoff == 0 || shnum == 0) return out;
    const bool pie = etype == kEtDyn;

    for (std::uint16_t si = 0; si < shnum; ++si) {
        const std::size_t sh = static_cast<std::size_t>(shoff) +
                               static_cast<std::size_t>(si) * shentsize;
        std::uint32_t type = 0;
        std::uint64_t offset = 0, shsize = 0, entsize = 0;
        std::uint32_t link = 0;
        if (!rd(data, size, sh + 0x04, type) ||  // sh_type
            !rd(data, size, sh + 0x18, offset) || // sh_offset
            !rd(data, size, sh + 0x20, shsize) || // sh_size
            !rd(data, size, sh + 0x28, link) ||   // sh_link (strtab index)
            !rd(data, size, sh + 0x38, entsize))  // sh_entsize
            continue;
        if (type != kShtSymtab && type != kShtDynsym) continue;
        if (entsize == 0 || entsize < 24) entsize = 24;
        if (link >= shnum) continue;

        // The linked string table holds the symbol names.
        const std::size_t strsh = static_cast<std::size_t>(shoff) +
                                  static_cast<std::size_t>(link) * shentsize;
        std::uint64_t stroff = 0, strsz = 0;
        if (!rd(data, size, strsh + 0x18, stroff) ||
            !rd(data, size, strsh + 0x20, strsz))
            continue;

        for (std::uint64_t e = 0; e + 24 <= shsize; e += entsize) {
            const std::size_t sym = static_cast<std::size_t>(offset + e);
            std::uint32_t st_name = 0;
            std::uint16_t st_shndx = 0;
            std::uint64_t st_value = 0;
            if (!rd(data, size, sym + 0x00, st_name) ||
                !rd(data, size, sym + 0x06, st_shndx) ||
                !rd(data, size, sym + 0x08, st_value))
                break;
            if (st_name == 0 || st_shndx == 0) continue; // unnamed / undef

            // Name: NUL-terminated string inside the strtab.
            if (st_name >= strsz) continue;
            const char* s = reinterpret_cast<const char*>(data + stroff +
                                                          st_name);
            const std::size_t avail = static_cast<std::size_t>(strsz - st_name);
            const void* nul = std::memchr(s, '\0', avail);
            if (nul == nullptr) continue; // unterminated — corrupt
            const std::size_t len =
                static_cast<const char*>(nul) - s;
            if (len == 0) continue;
            out[std::string(s, len)] = pie ? base + st_value : st_value;
        }
    }
    return out;
}

std::optional<int> Dynlib::load(GuestMemory& mem, std::string name,
                                const std::byte* data, std::size_t size) {
    auto image = loader::parse_elf64(data, size);
    if (!image) {
        log::error("dynlib", "load ", name, ": not a valid ELF64 image");
        return std::nullopt;
    }

    // Span covered by PT_LOAD segments (page-rounded, relative to vaddr 0
    // for PIE images).
    std::uint64_t lo = ~0ull, hi = 0;
    for (const auto& seg : image->segments) {
        if (seg.type != kPtLoad || seg.memsz == 0) continue;
        lo = std::min(lo, page_down(seg.vaddr));
        hi = std::max(hi, page_up(seg.vaddr + seg.memsz));
    }
    if (lo == ~0ull) {
        log::error("dynlib", "load ", name, ": no PT_LOAD segments");
        return std::nullopt;
    }

    const std::uint64_t base = next_base_;
    const std::uint64_t span = hi - lo;
    if (!mem.map(base, span)) {
        log::error("dynlib", "load ", name, ": cannot map ", span, " bytes");
        return std::nullopt;
    }
    for (const auto& seg : image->segments) {
        if (seg.type != kPtLoad || seg.filesz == 0) continue;
        if (seg.offset > size || seg.filesz > size - seg.offset) {
            log::error("dynlib", "load ", name, ": segment past end of image");
            mem.unmap(base, span);
            return std::nullopt;
        }
        // BSS tail past filesz stays zero (the fresh mapping is zeroed).
        mem.write(base + seg.vaddr - lo, data + seg.offset, seg.filesz);
    }

    Module m;
    m.id = next_id_;
    m.name = std::move(name);
    m.base = base;
    m.size = span;
    // Symbols are vaddr-relative in the image; rebase to the load base.
    // NOTE (predicted): assumes vaddr-0-based PIE layout — verify per-module
    // against real sprx when the decryption wall falls.
    m.symbols = parse_symbols(data, size, base - lo);
    ++next_id_;
    next_base_ += span + 0x100000; // guard gap between modules

    log::debug("dynlib", "loaded ", m.name, " id ", m.id, " base 0x",
               std::hex, base, std::dec, " (", m.symbols.size(),
               " symbols)");
    modules_.push_back(std::move(m));
    return modules_.back().id;
}

std::optional<std::uint64_t> Dynlib::resolve(int handle,
                                             std::string_view name) const {
    for (const auto& m : modules_) {
        if (m.id != handle) continue;
        const auto it = m.symbols.find(name);
        if (it == m.symbols.end()) return std::nullopt;
        return it->second;
    }
    return std::nullopt;
}

bool Dynlib::unload(GuestMemory& mem, int handle) {
    for (auto it = modules_.begin(); it != modules_.end(); ++it) {
        if (it->id != handle) continue;
        mem.unmap(it->base, it->size);
        log::debug("dynlib", "unloaded ", it->name, " id ", handle);
        modules_.erase(it);
        return true;
    }
    return false;
}

} // namespace kura::kernel
