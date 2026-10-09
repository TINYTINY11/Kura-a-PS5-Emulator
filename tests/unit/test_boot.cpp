// M2 exit-criterion test: a synthetic ELF is parsed, mapped into guest
// memory, and the interpreter executes it to completion — the full
// "file -> memory -> CPU" path without any real firmware involved.

#include "cpu/interpreter.hpp"
#include "loader/elf.hpp"

#include <iostream>
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

std::vector<std::byte> make_elf(const std::vector<std::uint8_t>& code,
                                std::uint64_t entry) {
    const std::size_t code_off = 0x100;
    std::vector<std::byte> b(code_off + code.size(), std::byte{0});
    auto put = [&](std::size_t off, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i)
            b[off + static_cast<std::size_t>(i)] =
                static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    };
    b[0] = std::byte{0x7F}; b[1] = std::byte{'E'};
    b[2] = std::byte{'L'};  b[3] = std::byte{'F'};
    b[4] = std::byte{2};    b[5] = std::byte{1};
    b[6] = std::byte{1};
    put(16, 2, 2);          // ET_EXEC
    put(18, 0x3E, 2);       // EM_X86_64
    put(20, 1, 4);
    put(24, entry, 8);      // e_entry
    put(32, 64, 8);         // e_phoff
    put(52, 64, 2);
    put(54, 56, 2);
    put(56, 1, 2);          // one program header
    put(64 + 0, 1, 4);      // PT_LOAD
    put(64 + 4, 5, 4);      // R+X
    put(64 + 8, code_off, 8);
    put(64 + 16, entry, 8);
    put(64 + 24, entry, 8);
    put(64 + 32, code.size(), 8);
    put(64 + 40, 0x1000, 8);
    put(64 + 48, 0x1000, 8);
    for (std::size_t i = 0; i < code.size(); ++i)
        b[code_off + i] = static_cast<std::byte>(code[i]);
    return b;
}

void test_elf_boots_and_exits() {
    // A tiny guest "program": sum 1..10 in a loop, then exit(= result)
    //   ecx=0; edx=10; loop: ecx+=edx; edx--; cmp edx,0; jnz loop;
    //   mov rdi,ecx; mov rax,1; syscall   (exit code = ecx)
    const std::vector<std::uint8_t> code = {
        0xB9, 0x00, 0x00, 0x00, 0x00,       // mov ecx, 0
        0xBA, 0x0A, 0x00, 0x00, 0x00,       // mov edx, 10
        0x48, 0x01, 0xD1,                   // add rcx, rdx
        0x48, 0xFF, 0xCA,                   // dec rdx
        0x48, 0x83, 0xFA, 0x00,             // cmp rdx, 0
        0x75, 0xF4,                         // jnz -12
        0x48, 0x89, 0xCF,                   // mov rdi, rcx
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1 (exit)
        0x0F, 0x05,                         // syscall
    };

    const std::uint64_t entry = 0x400000;
    auto elf = make_elf(code, entry);

    auto img = kura::loader::parse_elf64(elf.data(), elf.size());
    CHECK(img.has_value());
    if (!img) return;

    kura::GuestMemory mem;
    CHECK(kura::loader::load_into(*img, elf.data(), elf.size(), mem));
    CHECK(mem.map(0x7FFF0000ull, 0x10000)); // stack

    kura::cpu::CpuState cpu;
    cpu.rip = img->entry;
    cpu.gpr[kura::cpu::RSP] = 0x80000000ull;

    kura::cpu::Interpreter interp(mem, cpu);
    int exit_code = -1;
    interp.set_exit_hook(
        [](kura::cpu::CpuState&, int code, void* user) {
            *static_cast<int*>(user) = code;
            return false;
        },
        &exit_code);

    auto r = interp.run(100000);
    CHECK(r.reason == kura::cpu::StopReason::Exited);
    CHECK(exit_code == 55); // 10+9+...+1 computed by GUEST code
    CHECK(r.steps >= 10);   // it really executed an instruction stream
}

} // namespace

int main() {
    test_elf_boots_and_exits();
    if (g_failures == 0) std::cout << "test_boot: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
