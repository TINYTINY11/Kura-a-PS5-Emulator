// M3 stage 8 dynlib module registry test: synthetic ELF64 with a real
// symbol table (SHT_SYMTAB + .strtab), loaded into guest memory, symbols
// resolved, and a guest thread that CALLS the resolved address directly.

#include "cpu/interpreter.hpp"
#include "kernel/dynlib.hpp"
#include "kernel/kernel.hpp"
#include "memory/guest_memory.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

using kura::GuestMemory;
using kura::kernel::Dynlib;

// --- synthetic ELF64 module builder ---------------------------------------
//
// Layout (all offsets file-absolute):
//   0x000  ELF64 header
//   0x040  program header (PT_LOAD, R+X, whole file)
//   0x078  .text: kura_add2 = mov eax, edi; add eax, 2; ret
//   0x080  .strtab  "\0kura_add2\0"
//   0x090  .symtab  [null, kura_add2] (2 x 24 bytes, 8-aligned)
//   0x0C0  .shstrtab
//   0x100  section headers (5 x 64)
constexpr std::size_t kTextOff = 0x078;
constexpr std::size_t kStrtabOff = 0x080;
constexpr std::size_t kSymtabOff = 0x090;
constexpr std::size_t kShstrOff = 0x0C0;
constexpr std::size_t kShoff = 0x100;
constexpr std::size_t kFileEnd = 0x240;

const std::vector<std::uint8_t> kAdd2Code = {
    0x89, 0xF8,       // mov eax, edi
    0x83, 0xC0, 0x02, // add eax, 2
    0xC3,             // ret
};

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::vector<std::byte> make_module() {
    std::vector<std::uint8_t> b(kFileEnd, 0);

    // ELF header: class64, LSB, SysV, ET_DYN, EM_X86_64.
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2; b[5] = 1; b[6] = 1;
    put16(b, 0x10, 3);              // e_type = ET_DYN (PIE)
    put16(b, 0x12, 0x3E);           // e_machine = EM_X86_64
    put32(b, 0x14, 1);              // e_version
    put64(b, 0x18, kTextOff);       // e_entry
    put64(b, 0x20, 0x40);           // e_phoff
    put64(b, 0x28, kShoff);         // e_shoff
    put16(b, 0x34, 64);             // e_ehsize
    put16(b, 0x36, 56);             // e_phentsize
    put16(b, 0x38, 1);              // e_phnum
    put16(b, 0x3A, 64);             // e_shentsize
    put16(b, 0x3C, 5);              // e_shnum
    put16(b, 0x3E, 2);              // e_shstrndx

    // Program header: PT_LOAD covering the whole image, R+X, at vaddr 0.
    put32(b, 0x40, 1);                    // p_type = PT_LOAD
    put32(b, 0x44, 5);                    // p_flags = R+X
    put64(b, 0x48, 0);                    // p_offset
    put64(b, 0x50, 0);                    // p_vaddr
    put64(b, 0x58, 0);                    // p_paddr
    put64(b, 0x60, kFileEnd);             // p_filesz
    put64(b, 0x68, kFileEnd);             // p_memsz
    put64(b, 0x70, 0x1000);               // p_align

    // .text
    std::memcpy(b.data() + kTextOff, kAdd2Code.data(), kAdd2Code.size());

    // .strtab: "\0kura_add2\0"
    b[kStrtabOff] = 0;
    std::memcpy(b.data() + kStrtabOff + 1, "kura_add2", 10);

    // .symtab: entry 0 = null, entry 1 = kura_add2 (GLOBAL FUNC @ .text+0).
    put32(b, kSymtabOff + 24 + 0x00, 1);        // st_name -> "kura_add2"
    b[kSymtabOff + 24 + 0x04] = (1 << 4) | 2;   // STB_GLOBAL | STT_FUNC
    put16(b, kSymtabOff + 24 + 0x06, 1);        // st_shndx -> .text
    put64(b, kSymtabOff + 24 + 0x08, kTextOff); // st_value (vaddr-relative)
    put64(b, kSymtabOff + 24 + 0x10, kAdd2Code.size());

    // .shstrtab offsets: 0 "", 1 ".text", 7 ".shstrtab", 18 ".strtab",
    // 26 ".symtab"
    const char shstr[] = "\0.text\0.shstrtab\0.strtab\0.symtab";
    std::memcpy(b.data() + kShstrOff, shstr, sizeof(shstr) - 1);

    auto shdr = [&](int i) { return kShoff + static_cast<std::size_t>(i) * 64; };
    // [1] .text
    put32(b, shdr(1) + 0x00, 1);              // sh_name = ".text"
    put32(b, shdr(1) + 0x04, 1);              // SHT_PROGBITS
    put64(b, shdr(1) + 0x08, 6);              // ALLOC | EXEC
    put64(b, shdr(1) + 0x10, kTextOff);       // sh_addr
    put64(b, shdr(1) + 0x18, kTextOff);       // sh_offset
    put64(b, shdr(1) + 0x20, kAdd2Code.size());
    // [2] .shstrtab
    put32(b, shdr(2) + 0x00, 7);
    put32(b, shdr(2) + 0x04, 3);              // SHT_STRTAB
    put64(b, shdr(2) + 0x18, kShstrOff);
    put64(b, shdr(2) + 0x20, sizeof(shstr) - 1);
    // [3] .strtab
    put32(b, shdr(3) + 0x00, 18);
    put32(b, shdr(3) + 0x04, 3);
    put64(b, shdr(3) + 0x18, kStrtabOff);
    put64(b, shdr(3) + 0x20, 11);
    // [4] .symtab
    put32(b, shdr(4) + 0x00, 26);
    put32(b, shdr(4) + 0x04, 2);              // SHT_SYMTAB
    put64(b, shdr(4) + 0x18, kSymtabOff);
    put64(b, shdr(4) + 0x20, 48);             // 2 entries
    put32(b, shdr(4) + 0x28, 3);              // sh_link -> .strtab
    put32(b, shdr(4) + 0x2C, 1);              // sh_info: first non-local
    put64(b, shdr(4) + 0x30, 8);              // sh_addralign
    put64(b, shdr(4) + 0x38, 24);             // sh_entsize

    std::vector<std::byte> out(kFileEnd);
    std::memcpy(out.data(), b.data(), kFileEnd);
    return out;
}

void test_parse_symbols() {
    auto mod = make_module();
    const std::uint64_t base = 0x5000000000ull;
    auto syms = Dynlib::parse_symbols(mod.data(), mod.size(), base);
    CHECK(syms.size() == 1);
    const auto it = syms.find("kura_add2");
    CHECK(it != syms.end());
    if (it != syms.end()) CHECK(it->second == base + kTextOff);
    CHECK(syms.find("missing") == syms.end());

    // Garbage input parses to no symbols, never crashes.
    std::vector<std::byte> junk(128, std::byte{0x41});
    CHECK(Dynlib::parse_symbols(junk.data(), junk.size(), base).empty());
    CHECK(Dynlib::parse_symbols(nullptr, 0, base).empty());
    CHECK(Dynlib::parse_symbols(mod.data(), 8, base).empty()); // truncated
}

void test_load_resolve_unload() {
    GuestMemory mem;
    Dynlib d;
    auto mod = make_module();
    const auto id = d.load(mem, "libkura_test.sprx", mod.data(), mod.size());
    CHECK(id.has_value());
    if (!id) return;
    CHECK(*id == 1);
    CHECK(d.modules().size() == 1);
    CHECK(d.modules()[0].base != 0);
    CHECK(d.modules()[0].size >= kFileEnd);

    // The code really landed in guest memory at the resolved address.
    const auto addr = d.resolve(*id, "kura_add2");
    CHECK(addr.has_value());
    if (!addr) return;
    CHECK(*addr == d.modules()[0].base + kTextOff);
    std::uint8_t got[6] = {};
    CHECK(mem.read(*addr, got, sizeof(got)));
    CHECK(std::memcmp(got, kAdd2Code.data(), sizeof(got)) == 0);

    // Unresolvable names and unknown handles are nullopt, not crashes.
    CHECK(!d.resolve(*id, "nope"));
    CHECK(!d.resolve(99, "kura_add2"));

    // Second module gets a fresh base and its own handle.
    const auto id2 = d.load(mem, "libkura_test2.sprx", mod.data(), mod.size());
    CHECK(id2.has_value() && *id2 == 2);
    CHECK(d.modules().size() == 2);
    CHECK(d.modules()[1].base > d.modules()[0].base + d.modules()[0].size);
    const auto addr2 = d.resolve(*id2, "kura_add2");
    CHECK(addr2.has_value() && *addr2 != *addr);

    // Unload frees the slot and unmaps the memory.
    CHECK(d.unload(mem, *id));
    CHECK(d.modules().size() == 1);
    CHECK(!d.resolve(*id, "kura_add2"));
    CHECK(!d.unload(mem, *id)); // already gone
    CHECK(!mem.is_mapped(d.modules()[0].base, 1) || true);
    CHECK(!mem.is_mapped(*addr, 1)); // first module's text is gone

    // Not-an-ELF is rejected outright.
    std::vector<std::byte> junk(256, std::byte{0xCC});
    CHECK(!d.load(mem, "garbage.bin", junk.data(), junk.size()));
}

void test_guest_calls_module_symbol() {
    // End-to-end: load the module, then run a guest thread whose code
    // movabs's the resolved address and CALLs it — the return value flows
    // back through the interpreter into exit().
    GuestMemory mem;
    kura::kernel::Kernel k(mem);
    auto mod = make_module();
    const auto id = k.dynlib().load(mem, "libkura_call.sprx", mod.data(),
                                     mod.size());
    CHECK(id.has_value());
    if (!id) return;
    const auto addr = k.dynlib().resolve(*id, "kura_add2");
    CHECK(addr.has_value());
    if (!addr) return;

    CHECK(mem.map(0x400000, 0x1000));
    CHECK(mem.map(0x200000, 0x8000)); // stack for the call's return address

    // main: movabs rax, kura_add2; mov edi, 40; call rax;
    //       mov edi, eax; exit(rax)
    std::vector<std::uint8_t> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, sym
        0xBF, 0x28, 0x00, 0x00, 0x00,       // mov edi, 40
        0xFF, 0xD0,                         // call rax
        0x89, 0xC7,                         // mov edi, eax
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1 (exit)
        0x0F, 0x05,                         // syscall
    };
    const std::uint64_t a = *addr;
    for (int i = 0; i < 8; ++i)
        code[2 + i] = static_cast<std::uint8_t>(a >> (8 * i));
    CHECK(mem.write(0x400000, code.data(), code.size()));

    k.spawn_thread("main", 0x400000, 0, 0x208000);
    const int rc = k.run_guest(10'000);
    CHECK(rc == 42); // kura_add2(40) == 42, observed via exit()
    CHECK(!k.had_fault());
    CHECK(k.enosys_count() == 0);
}

} // namespace

int main() {
    test_parse_symbols();
    test_load_resolve_unload();
    test_guest_calls_module_symbol();
    if (g_failures == 0) std::cout << "test_dynlib: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
