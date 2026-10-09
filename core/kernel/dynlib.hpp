#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "memory/guest_memory.hpp"

namespace kura::kernel {

// A loaded guest module (shared library / SELF component).
struct Module {
    int id = 0;
    std::string name;
    std::uint64_t base = 0; // where PT_LOAD segments were mapped
    std::uint64_t size = 0; // total span (page-rounded)
    // exported symbols -> absolute guest addresses (rebased for PIE)
    std::map<std::string, std::uint64_t, std::less<>> symbols;
};

// Module registry (M3 stage 8). PS5 userland loads hundreds of libSce*.sprx
// modules through the sys_dynlib_* syscall family. The syscall NUMBERS for
// that family are Sony-specific and not yet verified (docs/SYSCALLS.md
// policy: no fabricated numbers in active dispatch — a wrong guess would
// execute an UNRELATED handler, which is worse than a graceful ENOSYS). So
// M3 builds the machinery itself: ELF symbol-table parsing, guest-memory
// mapping, handle-based dlsym. When real-binary analysis pins the numbers,
// the syscalls become thin wrappers over this class.
//
// Host safety (§11): load() copies bytes from a caller-provided buffer into
// guest memory only — it never maps host files and never executes anything
// on the host.
class Dynlib {
public:
    // Maps an ELF64 image's PT_LOAD segments into guest memory (bump
    // allocator above the kernel heap), indexes its symbol tables
    // (SHT_SYMTAB + SHT_DYNSYM), and returns a handle >= 1. nullopt on
    // parse failure or if a segment cannot be mapped.
    std::optional<int> load(GuestMemory& mem, std::string name,
                            const std::byte* data, std::size_t size);

    // dlsym: absolute guest address of an exported symbol.
    std::optional<std::uint64_t> resolve(int handle,
                                         std::string_view name) const;

    // dlclose: unmaps the module's span and forgets its symbols.
    bool unload(GuestMemory& mem, int handle);

    const std::vector<Module>& modules() const { return modules_; }

    // Symbol extraction from an ELF64 image (SHT_SYMTAB + SHT_DYNSYM).
    // ET_DYN (PIE) values are rebased to `base`; ET_EXEC values are
    // absolute and pass through. Undefined (st_shndx == 0) and unnamed
    // entries are skipped. Exposed for direct testing.
    static std::map<std::string, std::uint64_t, std::less<>>
    parse_symbols(const std::byte* data, std::size_t size,
                  std::uint64_t base);

private:
    std::vector<Module> modules_;
    std::uint64_t next_base_ = 0x4000000000ull; // 256 GiB: above kernel heap
    int next_id_ = 1;
};

} // namespace kura::kernel
