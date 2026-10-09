// M3 stage 1 kernel HLE test: syscalls driven directly through the
// dispatch entry points, plus one full "guest executes SYSCALL" run
// through the interpreter with the kernel installed.

#include "cpu/interpreter.hpp"
#include "kernel/kernel.hpp"
#include "kernel/syscalls.hpp"
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
using kura::kernel::Kernel;
namespace cpu = kura::cpu;
namespace sys = kura::kernel::sys;

constexpr std::uint64_t kScratch = 0x100000; // mapped test buffer

// Build a CpuState positioned for a syscall: number in RAX, args in the
// amd64 order (RDI, RSI, RDX, R10, R8).
cpu::CpuState st_for(std::uint64_t nr, std::uint64_t a0 = 0,
                     std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                     std::uint64_t a3 = 0, std::uint64_t a4 = 0) {
    cpu::CpuState st{};
    st.gpr[cpu::RAX] = nr;
    st.gpr[cpu::RDI] = a0;
    st.gpr[cpu::RSI] = a1;
    st.gpr[cpu::RDX] = a2;
    st.gpr[cpu::R10] = a3;
    st.gpr[cpu::R8] = a4;
    return st;
}

std::int64_t call(Kernel& k, cpu::CpuState& st) {
    k.on_syscall(st);
    return static_cast<std::int64_t>(st.gpr[cpu::RAX]);
}

void write_cstr(GuestMemory& mem, std::uint64_t at, const char* s) {
    CHECK(mem.write(at, s, std::strlen(s) + 1));
}

void test_getpid_and_spawn() {
    GuestMemory mem;
    Kernel k(mem);

    auto st = st_for(sys::kGetpid);
    CHECK(call(k, st) == 1); // init process

    auto& w = k.spawn_process("worker", 42);
    CHECK(w.ppid == 1);
    st = st_for(sys::kGetpid);
    CHECK(call(k, st) == 42);
    CHECK(k.current() != nullptr && k.current()->name == "worker");
}

void test_unknown_syscall_is_enosys() {
    GuestMemory mem;
    Kernel k(mem);
    auto st = st_for(9999);
    CHECK(call(k, st) == -sys::kENOSYS);
}

void test_anon_mmap_round_trip() {
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kScratch, 0x1000));

    // mmap(NULL, 0x2000, PROT_READ|WRITE, MAP_PRIVATE|MAP_ANON, -1, 0)
    const std::uint64_t want = k.heap_base(); // captured pre-bump
    auto st = st_for(sys::kMmap, 0, 0x2000, 3, 0x0002 | 0x1000, 0);
    const std::int64_t at = call(k, st);
    CHECK(at == static_cast<std::int64_t>(want));

    // The mapping is real: guest-memory access through it works.
    CHECK(mem.write_value(at, std::uint64_t{0xDEADBEEF}));
    std::uint64_t back = 0;
    CHECK(mem.read_value(at, back));
    CHECK(back == 0xDEADBEEF);

    // A second mmap lands beyond the first (bump allocator).
    st = st_for(sys::kMmap, 0, 0x1000, 3, 0x0002 | 0x1000, 0);
    const std::int64_t at2 = call(k, st);
    CHECK(at2 >= at + 0x2000);

    // munmap releases it.
    st = st_for(sys::kMunmap, static_cast<std::uint64_t>(at), 0x2000);
    CHECK(call(k, st) == 0);
    CHECK(!mem.is_mapped(static_cast<std::uint64_t>(at), 0x1000));

    // Zero-length mmap is EINVAL; a bad munmap address is EINVAL.
    st = st_for(sys::kMmap, 0, 0, 3, 0x1000, 0);
    CHECK(call(k, st) == -sys::kEINVAL);
    st = st_for(sys::kMunmap, 0x1234, 0x1000); // never mapped, unaligned
    CHECK(call(k, st) == -sys::kEINVAL);
}

void test_open_write_lseek_read() {
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kScratch, 0x1000));
    write_cstr(mem, kScratch + 0x00, "/data/hello.txt");
    CHECK(mem.write(kScratch + 0x100, "Hello Kura", 10));

    // open(path, O_CREAT|O_WRONLY)
    auto st = st_for(sys::kOpen, kScratch,
                     kura::kernel::kOCreat | kura::kernel::kOWrite);
    const std::int64_t fd = call(k, st);
    CHECK(fd >= 3);

    // write 10 bytes
    st = st_for(sys::kWrite, static_cast<std::uint64_t>(fd),
                kScratch + 0x100, 10);
    CHECK(call(k, st) == 10);

    // Named store sees the bytes (the fd bug this test exists to catch).
    auto bytes = k.vfs().file_bytes("/data/hello.txt");
    CHECK(bytes.has_value());
    CHECK(bytes && bytes->size() == 10);

    // rewind and read them back through the fd
    st = st_for(sys::kLseek, static_cast<std::uint64_t>(fd), 0,
                kura::kernel::kSeekSet);
    CHECK(call(k, st) == 0);
    st = st_for(sys::kRead, static_cast<std::uint64_t>(fd), kScratch + 0x200,
                10);
    CHECK(call(k, st) == 10);
    char back[11] = {};
    CHECK(mem.read(kScratch + 0x200, back, 10));
    CHECK(std::string(back, 10) == "Hello Kura");

    // close is idempotent-safe: first close works, second is EBADF
    st = st_for(sys::kClose, static_cast<std::uint64_t>(fd));
    CHECK(call(k, st) == 0);
    st = st_for(sys::kClose, static_cast<std::uint64_t>(fd));
    CHECK(call(k, st) == -sys::kEBADF);

    // opening a missing file without O_CREAT is ENOENT
    write_cstr(mem, kScratch + 0x40, "/data/nope.txt");
    st = st_for(sys::kOpen, kScratch + 0x40, kura::kernel::kORead);
    CHECK(call(k, st) == -sys::kENOENT);

    // writing to a bad fd is EBADF; writing to stdin is EBADF
    st = st_for(sys::kWrite, 999, kScratch + 0x100, 10);
    CHECK(call(k, st) == -sys::kEBADF);
    st = st_for(sys::kWrite, 0, kScratch + 0x100, 10);
    CHECK(call(k, st) == -sys::kEBADF);
}

void test_tty_capture_and_gettimeofday() {
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kScratch, 0x1000));
    CHECK(mem.write(kScratch, "guest says hi", 13));

    auto st = st_for(sys::kWrite, 1, kScratch, 13); // stdout
    CHECK(call(k, st) == 13);
    CHECK(k.vfs().tty_text().find("guest says hi") != std::string::npos);

    // gettimeofday(tv, NULL) fills a struct timeval in guest memory
    st = st_for(sys::kGettimeofday, kScratch + 0x400);
    CHECK(call(k, st) == 0);
    std::int64_t sec = 0;
    CHECK(mem.read_value(kScratch + 0x400, sec));
    CHECK(sec > 1'700'000'000); // a plausible wall clock
    st = st_for(sys::kGettimeofday, 0); // NULL tv is EFAULT
    CHECK(call(k, st) == -sys::kEFAULT);
}

void test_guest_program_syscalls_end_to_end() {
    // Guest code: write(1, msg, len); exit(7) — exercised through the
    // interpreter with Kernel::install() wiring the hooks.
    const std::vector<std::uint8_t> code = {
        0x48, 0xC7, 0xC0, 0x04, 0x00, 0x00, 0x00, // mov rax, 4  (write)
        0x48, 0xC7, 0xC7, 0x01, 0x00, 0x00, 0x00, // mov rdi, 1  (stdout)
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, msg (patched)
        0x48, 0xC7, 0xC2, 0x0D, 0x00, 0x00, 0x00, // mov rdx, 13
        0x0F, 0x05,                               // syscall
        0x48, 0xC7, 0xC7, 0x07, 0x00, 0x00, 0x00, // mov rdi, 7  (exit code)
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1  (exit)
        0x0F, 0x05,                               // syscall
    };

    const std::uint64_t entry = 0x400000;
    const std::uint64_t msg_addr = entry + code.size();

    // Hand-mapped code+message region (no ELF needed for this path).
    std::vector<std::byte> seg(code.size() + 16, std::byte{0});
    std::memcpy(seg.data(), code.data(), code.size());
    std::memcpy(seg.data() + code.size(), "Hello kernel\n", 13);
    // patch the mov rsi, imm32 (48 C7 C6 <imm32> starts at byte 14,
    // immediate at byte 17)
    for (int i = 0; i < 4; ++i)
        seg[17 + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((msg_addr >> (8 * i)) & 0xFF);

    GuestMemory mem;
    CHECK(mem.map(entry, 0x1000));
    CHECK(mem.write(entry, seg.data(), seg.size()));
    CHECK(mem.map(0x7FFF0000ull, 0x10000)); // stack

    cpu::CpuState cpu_state{};
    cpu_state.rip = entry;
    cpu_state.gpr[cpu::RSP] = 0x80000000ull;

    Kernel k(mem);
    k.spawn_process("guest", 77);
    cpu::Interpreter interp(mem, cpu_state);
    k.install(interp);

    auto r = interp.run(100000);
    CHECK(r.reason == cpu::StopReason::Exited);
    CHECK(r.exit_code == 7);
    CHECK(k.has_exited());
    CHECK(k.exit_code() == 7);
    CHECK(k.vfs().tty_text().find("Hello kernel") != std::string::npos);
}

} // namespace

int main() {
    test_getpid_and_spawn();
    test_unknown_syscall_is_enosys();
    test_anon_mmap_round_trip();
    test_open_write_lseek_read();
    test_tty_capture_and_gettimeofday();
    test_guest_program_syscalls_end_to_end();
    if (g_failures == 0) std::cout << "test_kernel: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
