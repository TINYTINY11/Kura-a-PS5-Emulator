// M3 stage 1 kernel HLE test: syscalls driven directly through the
// dispatch entry points, plus one full "guest executes SYSCALL" run
// through the interpreter with the kernel installed.

#include "cpu/interpreter.hpp"
#include "kernel/kernel.hpp"
#include "kernel/syscalls.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <chrono>
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
// amd64 order (RDI, RSI, RDX, R10, R8, R9).
cpu::CpuState st_for(std::uint64_t nr, std::uint64_t a0 = 0,
                     std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                     std::uint64_t a3 = 0, std::uint64_t a4 = 0,
                     std::uint64_t a5 = 0) {
    cpu::CpuState st{};
    st.gpr[cpu::RAX] = nr;
    st.gpr[cpu::RDI] = a0;
    st.gpr[cpu::RSI] = a1;
    st.gpr[cpu::RDX] = a2;
    st.gpr[cpu::R10] = a3;
    st.gpr[cpu::R8] = a4;
    st.gpr[cpu::R9] = a5;
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

void test_identity_and_ppid() {
    GuestMemory mem;
    Kernel k(mem);
    for (std::uint64_t nr : {sys::kGetuid, sys::kGeteuid, sys::kGetgid,
                             sys::kGetegid}) {
        auto st = st_for(nr);
        CHECK(call(k, st) == 0); // predicted root placeholder
    }
    auto st = st_for(sys::kGetppid);
    CHECK(call(k, st) == 0); // init has no parent
    k.spawn_process("child", 5);
    st = st_for(sys::kGetppid);
    CHECK(call(k, st) == 1); // child's parent is init
}

void test_clock_gettime_and_nanosleep() {
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kScratch, 0x1000));

    // CLOCK_REALTIME fills a plausible wall clock
    auto st = st_for(sys::kClockGettime, 0, kScratch);
    CHECK(call(k, st) == 0);
    std::int64_t sec = 0, nsec = 0;
    CHECK(mem.read_value(kScratch, sec));
    CHECK(mem.read_value(kScratch + 8, nsec));
    CHECK(sec > 1'700'000'000);
    CHECK(nsec >= 0 && nsec < 1'000'000'000);

    // CLOCK_MONOTONIC (FreeBSD id 4): overwrite with a sentinel first so
    // the check proves the kernel wrote something fresh
    CHECK(mem.write_value(kScratch + 0x40, std::int64_t{-1}));
    st = st_for(sys::kClockGettime, 4, kScratch + 0x40);
    CHECK(call(k, st) == 0);
    CHECK(mem.read_value(kScratch + 0x40, sec));
    CHECK(sec != -1);

    st = st_for(sys::kClockGettime, 99, kScratch);
    CHECK(call(k, st) == -sys::kEINVAL);

    // nanosleep(3ms) returns 0 and really waits (lower bound only —
    // host timer granularity varies)
    CHECK(mem.write_value(kScratch + 0x80, std::int64_t{0}));
    CHECK(mem.write_value(kScratch + 0x88, std::int64_t{3'000'000}));
    const auto t0 = std::chrono::steady_clock::now();
    st = st_for(sys::kNanosleep, kScratch + 0x80);
    CHECK(call(k, st) == 0);
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(waited >= std::chrono::milliseconds(1));

    // malformed timespec is EINVAL
    CHECK(mem.write_value(kScratch + 0x80, std::int64_t{-1}));
    st = st_for(sys::kNanosleep, kScratch + 0x80);
    CHECK(call(k, st) == -sys::kEINVAL);
}

void test_ioctl_enotty() {
    GuestMemory mem;
    Kernel k(mem);
    // a tty termios probe gets the FreeBSD non-tty answer for now
    auto st = st_for(sys::kIoctl, 1, 0x402C7413, 0);
    CHECK(call(k, st) == -sys::kENOTTY);
    st = st_for(sys::kIoctl, 999, 0, 0); // invalid fd
    CHECK(call(k, st) == -sys::kEBADF);
}

void test_sysctl() {
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kScratch, 0x1000));
    const auto put_i32 = [&](std::uint64_t off, std::int32_t v) {
        CHECK(mem.write_value(kScratch + off, v));
    };

    // kern.osrelease: size probe then fetch
    put_i32(0x00, 1); // CTL_KERN
    put_i32(0x04, 2); // KERN_OSRELEASE
    CHECK(mem.write_value(kScratch + 0x40, std::uint64_t{0}));
    auto st = st_for(sys::kSysctl, kScratch, 2, 0, kScratch + 0x40);
    CHECK(call(k, st) == 0);
    std::uint64_t need = 0;
    CHECK(mem.read_value(kScratch + 0x40, need));
    CHECK(need > 1);

    CHECK(mem.write_value(kScratch + 0x40, need));
    st = st_for(sys::kSysctl, kScratch, 2, kScratch + 0x100, kScratch + 0x40);
    CHECK(call(k, st) == 0);
    char s[64] = {};
    CHECK(mem.read(kScratch + 0x100, s, 64));
    CHECK(std::string(s).find("Kura") != std::string::npos);

    // hw.pagesize == 4096
    put_i32(0x20, 6); // CTL_HW
    put_i32(0x24, 7); // HW_PAGESIZE
    CHECK(mem.write_value(kScratch + 0x44, std::uint64_t{8}));
    st = st_for(sys::kSysctl, kScratch + 0x20, 2, kScratch + 0x200,
                kScratch + 0x44);
    CHECK(call(k, st) == 0);
    std::int32_t page = 0;
    CHECK(mem.read_value(kScratch + 0x200, page));
    CHECK(page == 4096);

    // unknown MIB -> ENOENT
    put_i32(0x30, 99);
    put_i32(0x34, 99);
    CHECK(mem.write_value(kScratch + 0x48, std::uint64_t{64}));
    st = st_for(sys::kSysctl, kScratch + 0x30, 2, kScratch + 0x240,
                kScratch + 0x48);
    CHECK(call(k, st) == -sys::kENOENT);

    // too-small buffer -> ENOMEM with the required length written back
    CHECK(mem.write_value(kScratch + 0x4C, std::uint64_t{1}));
    st = st_for(sys::kSysctl, kScratch, 2, kScratch + 0x280, kScratch + 0x4C);
    CHECK(call(k, st) == -sys::kENOMEM);
    std::uint64_t updated = 0;
    CHECK(mem.read_value(kScratch + 0x4C, updated));
    CHECK(updated == need);

    // writes to the tree -> EPERM (a4 = newp, a5 = newlen)
    st = st_for(sys::kSysctl, kScratch, 2, 0, kScratch + 0x40, kScratch, 8);
    CHECK(call(k, st) == -sys::kEPERM);
}

void test_stat_family() {
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kScratch, 0x1000));
    write_cstr(mem, kScratch, "/data/statme.txt");
    CHECK(mem.write(kScratch + 0x100, "abc", 3));

    // create + write through the syscall interface
    auto st = st_for(sys::kOpen, kScratch,
                     kura::kernel::kOCreat | kura::kernel::kOWrite);
    const std::int64_t fd = call(k, st);
    CHECK(fd >= 3);
    st = st_for(sys::kWrite, static_cast<std::uint64_t>(fd),
                kScratch + 0x100, 3);
    CHECK(call(k, st) == 3);

    // stat(path, buf) — offsets below lock the PREDICTED struct layout
    st = st_for(sys::kStat, kScratch, kScratch + 0x200);
    CHECK(call(k, st) == 0);
    std::uint64_t ino = 0, size = 0, blocks = 0, mtime = 0;
    std::uint32_t mode = 0, nlink = 0;
    CHECK(mem.read_value(kScratch + 0x200 + 8, ino));
    CHECK(mem.read_value(kScratch + 0x200 + 16, mode));
    CHECK(mem.read_value(kScratch + 0x200 + 20, nlink));
    CHECK(mem.read_value(kScratch + 0x200 + 56, mtime)); // mtim.tv_sec
    CHECK(mem.read_value(kScratch + 0x200 + 104, size));
    CHECK(mem.read_value(kScratch + 0x200 + 112, blocks));
    CHECK((mode & 0xF000) == kura::kernel::kS_IFREG);
    CHECK((mode & 0777) == 0644);
    CHECK(nlink == 1);
    CHECK(size == 3);
    CHECK(blocks == 1); // one 512-byte block
    CHECK(ino > 0);
    CHECK(mtime > 1'700'000'000); // stamped at write time

    // lstat: no symlinks in this Vfs, behaves like stat
    st = st_for(sys::kLstat, kScratch, kScratch + 0x300);
    CHECK(call(k, st) == 0);

    // missing path -> ENOENT
    write_cstr(mem, kScratch + 0x60, "/data/absent.txt");
    st = st_for(sys::kStat, kScratch + 0x60, kScratch + 0x300);
    CHECK(call(k, st) == -sys::kENOENT);

    // fstat on the regular fd -> same type/size story
    st = st_for(sys::kFstat, static_cast<std::uint64_t>(fd),
                kScratch + 0x400);
    CHECK(call(k, st) == 0);
    std::uint32_t fmode = 0;
    std::uint64_t fsize = 0;
    CHECK(mem.read_value(kScratch + 0x400 + 16, fmode));
    CHECK(mem.read_value(kScratch + 0x400 + 104, fsize));
    CHECK((fmode & 0xF000) == kura::kernel::kS_IFREG);
    CHECK(fsize == 3);

    // fstat on stdout -> character device with rw-rw-rw- predicted
    st = st_for(sys::kFstat, 1, kScratch + 0x500);
    CHECK(call(k, st) == 0);
    CHECK(mem.read_value(kScratch + 0x500 + 16, fmode));
    CHECK((fmode & 0xF000) == kura::kernel::kS_IFCHR);
    CHECK((fmode & 0777) == 0666);

    // fstat on an invalid fd -> EBADF
    st = st_for(sys::kFstat, 999, kScratch + 0x600);
    CHECK(call(k, st) == -sys::kEBADF);
}

void test_file_backed_mmap() {
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kScratch, 0x4000));

    // Build a 0x1800-byte pattern file through the syscall interface.
    std::vector<std::uint8_t> pattern(0x1800);
    for (std::size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    CHECK(mem.write(kScratch + 0x2000, pattern.data(), pattern.size()));
    write_cstr(mem, kScratch, "/mmap.bin");
    auto st = st_for(sys::kOpen, kScratch,
                     kura::kernel::kOCreat | kura::kernel::kORdwr);
    const std::int64_t fd = call(k, st);
    CHECK(fd >= 3);
    st = st_for(sys::kWrite, static_cast<std::uint64_t>(fd),
                kScratch + 0x2000, 0x1800);
    CHECK(call(k, st) == 0x1800);

    // mmap(NULL, 0x2000, PROT_RW, MAP_PRIVATE, fd, 0) — not anonymous
    st = st_for(sys::kMmap, 0, 0x2000, 3, 0x0002,
                static_cast<std::uint64_t>(fd), 0);
    const std::int64_t at = call(k, st);
    CHECK(at > 0);
    if (at <= 0) return;
    std::vector<std::uint8_t> got(0x2000);
    CHECK(mem.read(static_cast<std::uint64_t>(at), got.data(), 0x2000));
    CHECK(std::equal(got.begin(), got.begin() + 0x1800, pattern.begin()));
    bool tail_zero = true; // bytes past EOF stay zero-filled
    for (std::size_t i = 0x1800; i < got.size(); ++i)
        if (got[i] != 0) tail_zero = false;
    CHECK(tail_zero);

    // offset into the file: first mapped byte == pattern[0x1000]
    st = st_for(sys::kMmap, 0, 0x1000, 3, 0x0002,
                static_cast<std::uint64_t>(fd), 0x1000);
    const std::int64_t at2 = call(k, st);
    CHECK(at2 > 0);
    std::uint8_t first = 0;
    CHECK(mem.read_value(static_cast<std::uint64_t>(at2), first));
    CHECK(first == pattern[0x1000]);

    // rejections
    st = st_for(sys::kMmap, 0, 0x1000, 3, 0x0001, // MAP_SHARED file
                static_cast<std::uint64_t>(fd), 0);
    CHECK(call(k, st) == -sys::kENOSYS);
    st = st_for(sys::kMmap, 0, 0x1000, 3, 0x0002, 999, 0); // bad fd
    CHECK(call(k, st) == -sys::kEBADF);
    st = st_for(sys::kMmap, 0, 0x1000, 3, 0x0002,
                static_cast<std::uint64_t>(fd), 0x999999); // past EOF
    CHECK(call(k, st) == -sys::kEINVAL);
    st = st_for(sys::kMmap, 0, 0x1000, 3, 0x0002, static_cast<std::uint64_t>(-1),
                0); // non-anon, fd -1
    CHECK(call(k, st) == -sys::kEINVAL);
}

// --- M3 stage 6: cooperative scheduler over hand-assembled guest code -------

// Data layout shared by the threaded tests:
//   0x100000  thr_param structs / scratch
//   0x101000  counter (test 1), +4 = child thr_self slot
//   0x102000  ca (test 4, worker A's counter)
//   0x103000  cb (test 4, worker B's counter)
//   0x104000  done (test 4)
//   0x200000..0x208000  main stack (top 0x208000)
//   0x300000..          worker stacks (mapped by thr_new)
constexpr std::uint64_t kData = 0x100000;
constexpr std::uint64_t kCounter = 0x101000;
constexpr std::uint64_t kCa = 0x102000;
constexpr std::uint64_t kCb = 0x103000;
constexpr std::uint64_t kDone = 0x104000;
constexpr std::uint64_t kMainStackTop = 0x208000;

void write_param(GuestMemory& mem, std::uint64_t at, std::uint64_t arg,
                 std::uint64_t stack_base, std::uint64_t stack_size,
                 std::uint64_t child_fn) {
    CHECK(mem.write_value(at + 0, arg));
    CHECK(mem.write_value(at + 8, stack_base));
    CHECK(mem.write_value(at + 16, stack_size));
    CHECK(mem.write_value(at + 24, child_fn));
}

void test_multithreaded_guest_futex() {
    // The M3 exit criterion: a guest program that spawns a thread, blocks
    // on a futex, gets woken by the child, and observes the child's write.
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kData, 0x5000));
    CHECK(mem.map(0x400000, 0x1000));
    CHECK(mem.map(0x200000, 0x8000)); // main stack

    // main @ 0x400000: thr_new(param); for(;;) umtx WAIT(counter==0);
    //                 exit(counter)
    const std::vector<std::uint8_t> main_code = {
        0x48, 0xC7, 0xC7, 0x00, 0x00, 0x10, 0x00, // mov rdi, 0x100000
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0
        0x48, 0xC7, 0xC0, 0xAF, 0x01, 0x00, 0x00, // mov rax, 431 (thr_new)
        0x0F, 0x05,                               // syscall -> rax = tid
        0x48, 0xC7, 0xC3, 0x00, 0x10, 0x10, 0x00, // mov rbx, &counter
        // wait_loop:
        0x31, 0xFF,                               // xor edi, edi (owner NULL)
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0 (UMTX_OP_WAIT)
        0x48, 0xC7, 0xC2, 0x00, 0x10, 0x10, 0x00, // mov rdx, &counter
        0x49, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00, // mov r10, 0 (expected)
        0x4D, 0x31, 0xC0,                         // xor r8, r8 (no timeout)
        0x48, 0xC7, 0xC0, 0xC6, 0x01, 0x00, 0x00, // mov rax, 454 (__umtx_op)
        0x0F, 0x05,                               // syscall
        0x8B, 0x03,                               // mov eax, [rbx]
        0x85, 0xC0,                               // test eax, eax
        0x74, 0xD7,                               // je wait_loop
        0x89, 0xC7,                               // mov edi, eax (counter)
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1 (exit)
        0x0F, 0x05,                               // syscall
    };
    // child @ 0x400052: store thr_self() tid; counter = 42; umtx WAKE;
    //                   thr_exit(0) — the tid store happens BEFORE the wake
    //                   so main's exit() can never retire this thread first.
    const std::vector<std::uint8_t> child_code = {
        0x48, 0xC7, 0xC3, 0x00, 0x10, 0x10, 0x00, // mov rbx, &counter
        0x48, 0xC7, 0xC0, 0xC8, 0x01, 0x00, 0x00, // mov rax, 456 (thr_self)
        0x0F, 0x05,                               // syscall
        0x89, 0x43, 0x04,                         // mov [rbx+4], eax
        0xC7, 0x03, 0x2A, 0x00, 0x00, 0x00,       // mov dword [rbx], 42
        0x31, 0xFF,                               // xor edi, edi
        0x48, 0xC7, 0xC6, 0x01, 0x00, 0x00, 0x00, // mov rsi, 1 (UMTX_OP_WAKE)
        0x48, 0xC7, 0xC2, 0x00, 0x10, 0x10, 0x00, // mov rdx, &counter
        0x49, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00, // mov r10, 1 (wake 1)
        0x4D, 0x31, 0xC0,                         // xor r8, r8
        0x48, 0xC7, 0xC0, 0xC6, 0x01, 0x00, 0x00, // mov rax, 454
        0x0F, 0x05,                               // syscall
        0x31, 0xFF,                               // xor edi, edi
        0x48, 0xC7, 0xC0, 0xB0, 0x01, 0x00, 0x00, // mov rax, 432 (thr_exit)
        0x0F, 0x05,                               // syscall
    };
    CHECK(mem.write(0x400000, main_code.data(), main_code.size()));
    CHECK(mem.write(0x400052, child_code.data(), child_code.size()));
    write_param(mem, kData, 0, 0x300000, 0x8000, 0x400052);

    kura::kernel::Thread* main_t = k.spawn_thread("main", 0x400000, 0,
                                                  kMainStackTop);
    CHECK(main_t != nullptr);
    CHECK(main_t->tid == 1);

    const int rc = k.run_guest(100'000);
    CHECK(rc == 42);       // exit(counter) observed the child's write
    CHECK(!k.had_fault());
    std::uint32_t counter = 0, self_slot = 0;
    CHECK(mem.read_value(kCounter, counter));
    CHECK(counter == 42);  // child really ran and wrote guest memory
    CHECK(mem.read_value(kCounter + 4, self_slot));
    CHECK(self_slot == 2); // thr_self returned the child's tid
    CHECK(k.threads().size() == 2);
    for (const auto& t : k.threads())
        CHECK(t->status == kura::kernel::Thread::Status::Exited);
    CHECK(k.enosys_count() == 0); // syscall trace is clean
    // thr_new, WAIT | WAKE, thr_self, thr_exit — exit goes through the
    // exit hook, not the syscall hook, so it is not counted here.
    CHECK(k.syscall_count() == 5);
}

void test_scheduler_sleep() {
    // A sleeping thread blocks only itself; the scheduler host-sleeps until
    // its deadline when nothing else is runnable, then resumes it.
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kData, 0x1000));
    CHECK(mem.map(0x400000, 0x1000));
    CHECK(mem.map(0x200000, 0x8000));
    CHECK(mem.write_value<std::int64_t>(kData, 0));          // tv_sec
    CHECK(mem.write_value<std::int64_t>(kData + 8, 30'000'000)); // 30 ms

    const std::vector<std::uint8_t> code = {
        0x48, 0xC7, 0xC7, 0x00, 0x00, 0x10, 0x00, // mov rdi, 0x100000
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0 (no rem)
        0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00, // mov rax, 60 (nanosleep)
        0x0F, 0x05,                               // syscall
        0x48, 0xC7, 0xC7, 0x07, 0x00, 0x00, 0x00, // mov rdi, 7
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1 (exit)
        0x0F, 0x05,                               // syscall
    };
    CHECK(mem.write(0x400000, code.data(), code.size()));
    k.spawn_thread("main", 0x400000, 0, kMainStackTop);

    const auto t0 = std::chrono::steady_clock::now();
    const int rc = k.run_guest(1000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(rc == 7);
    CHECK(!k.had_fault());
    CHECK(ms >= 25);   // the sleep really happened
    CHECK(ms < 2000);  // and did not hang the host
    CHECK(k.enosys_count() == 0);
}

void test_futex_deadlock_detected() {
    // A lone thread futex-waits on a word nobody will ever wake. The
    // scheduler must report a deadlock instead of hanging the host.
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kData, 0x1000));   // kData stays zero — never written
    CHECK(mem.map(0x400000, 0x1000));
    CHECK(mem.map(0x200000, 0x8000));

    const std::vector<std::uint8_t> code = {
        0x31, 0xFF,                               // xor edi, edi
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0 (WAIT)
        0x48, 0xC7, 0xC2, 0x00, 0x00, 0x10, 0x00, // mov rdx, 0x100000
        0x49, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00, // mov r10, 0
        0x4D, 0x31, 0xC0,                         // xor r8, r8
        0x48, 0xC7, 0xC0, 0xC6, 0x01, 0x00, 0x00, // mov rax, 454
        0x0F, 0x05,                               // syscall — blocks forever
        0x48, 0xC7, 0xC7, 0x63, 0x00, 0x00, 0x00, // mov rdi, 99 (unreachable)
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
        0x0F, 0x05,
    };
    CHECK(mem.write(0x400000, code.data(), code.size()));
    k.spawn_thread("main", 0x400000, 0, kMainStackTop);

    const auto t0 = std::chrono::steady_clock::now();
    const int rc = k.run_guest(10'000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(k.had_fault()); // deadlock reported, host never hung
    CHECK(rc == 0);       // process never exited
    CHECK(ms < 2000);
}

// Worker: add [self],1 until [self]==25000; WAIT(peer != 0);
//         add [done],1; WAKE(done,1); thr_exit(0)
std::vector<std::uint8_t> worker_code(std::uint32_t self,
                                      std::uint32_t peer) {
    std::vector<std::uint8_t> c;
    auto b = [&](std::initializer_list<std::uint8_t> bytes) {
        c.insert(c.end(), bytes);
    };
    auto u32 = [&](std::uint32_t v) {
        c.push_back(static_cast<std::uint8_t>(v));
        c.push_back(static_cast<std::uint8_t>(v >> 8));
        c.push_back(static_cast<std::uint8_t>(v >> 16));
        c.push_back(static_cast<std::uint8_t>(v >> 24));
    };
    b({0x48, 0xC7, 0xC3}); u32(self);              // mov rbx, self
    const std::size_t loop = c.size();
    b({0x83, 0x03, 0x01});                         // add dword [rbx], 1
    b({0x81, 0x3B}); u32(25000);                   // cmp dword [rbx], 25000
    b({0x75});                                     // jne loop
    c.push_back(static_cast<std::uint8_t>(loop - (c.size() + 1)));
    // WAKE our own counter first: a sibling already blocked on it must be
    // released before we go wait on theirs, or both sleep forever.
    b({0x31, 0xFF});                               // xor edi, edi
    b({0x48, 0xC7, 0xC6, 0x01, 0x00, 0x00, 0x00}); // mov rsi, 1 (WAKE)
    b({0x48, 0xC7, 0xC2}); u32(self);              // mov rdx, self
    b({0x49, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00}); // mov r10, 1
    b({0x4D, 0x31, 0xC0});                         // xor r8, r8
    b({0x48, 0xC7, 0xC0, 0xC6, 0x01, 0x00, 0x00}); // mov rax, 454
    b({0x0F, 0x05});                               // syscall
    b({0x48, 0xC7, 0xC2}); u32(peer);              // mov rdx, peer
    const std::size_t wait = c.size();
    b({0x31, 0xFF});                               // xor edi, edi
    b({0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00}); // mov rsi, 0 (WAIT)
    b({0x49, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00}); // mov r10, 0
    b({0x4D, 0x31, 0xC0});                         // xor r8, r8
    b({0x48, 0xC7, 0xC0, 0xC6, 0x01, 0x00, 0x00}); // mov rax, 454
    b({0x0F, 0x05});                               // syscall
    b({0x83, 0x3A, 0x00});                         // cmp dword [rdx], 0
    b({0x74});                                     // je wait
    c.push_back(static_cast<std::uint8_t>(wait - (c.size() + 1)));
    b({0x48, 0xC7, 0xC3}); u32(static_cast<std::uint32_t>(kDone));
    b({0x83, 0x03, 0x01});                         // add dword [rbx], 1
    b({0x31, 0xFF});                               // xor edi, edi
    b({0x48, 0xC7, 0xC6, 0x01, 0x00, 0x00, 0x00}); // mov rsi, 1 (WAKE)
    b({0x48, 0xC7, 0xC2}); u32(static_cast<std::uint32_t>(kDone));
    b({0x49, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00}); // mov r10, 1
    b({0x4D, 0x31, 0xC0});                         // xor r8, r8
    b({0x48, 0xC7, 0xC0, 0xC6, 0x01, 0x00, 0x00}); // mov rax, 454
    b({0x0F, 0x05});                               // syscall
    b({0x31, 0xFF});                               // xor edi, edi
    b({0x48, 0xC7, 0xC0, 0xB0, 0x01, 0x00, 0x00}); // mov rax, 432 (thr_exit)
    b({0x0F, 0x05});                               // syscall
    return c;
}

void test_interleaved_cpu_bound_workers() {
    // Two CPU-bound workers that can only finish if the scheduler rotates
    // between them: each spins past several quanta on its own counter, then
    // waits for the sibling's. No interleaving => deadlock => test fails.
    GuestMemory mem;
    Kernel k(mem);
    CHECK(mem.map(kData, 0x5000));
    CHECK(mem.map(0x400000, 0x1000));
    CHECK(mem.map(0x200000, 0x8000));

    // main: thr_new(A); thr_new(B); wait for done==2; exit(7)
    const std::vector<std::uint8_t> main_code = {
        0x48, 0xC7, 0xC7, 0x00, 0x01, 0x10, 0x00, // mov rdi, 0x100100 (&paramA)
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0
        0x48, 0xC7, 0xC0, 0xAF, 0x01, 0x00, 0x00, // mov rax, 431 (thr_new)
        0x0F, 0x05,                               // syscall
        0x48, 0xC7, 0xC7, 0x40, 0x01, 0x10, 0x00, // mov rdi, 0x100140 (&paramB)
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0
        0x48, 0xC7, 0xC0, 0xAF, 0x01, 0x00, 0x00, // mov rax, 431
        0x0F, 0x05,                               // syscall
        0x48, 0xC7, 0xC3, 0x00, 0x40, 0x10, 0x00, // mov rbx, &done
        // wait_loop:
        0x31, 0xFF,                               // xor edi, edi
        0x48, 0xC7, 0xC6, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0 (WAIT)
        0x48, 0xC7, 0xC2, 0x00, 0x40, 0x10, 0x00, // mov rdx, &done
        0x49, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00, // mov r10, 0
        0x4D, 0x31, 0xC0,                         // xor r8, r8
        0x48, 0xC7, 0xC0, 0xC6, 0x01, 0x00, 0x00, // mov rax, 454
        0x0F, 0x05,                               // syscall
        0x8B, 0x03,                               // mov eax, [rbx]
        0x83, 0xF8, 0x02,                         // cmp eax, 2
        0x7C, 0xD6,                               // jl wait_loop
        0x48, 0xC7, 0xC7, 0x07, 0x00, 0x00, 0x00, // mov rdi, 7
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1 (exit)
        0x0F, 0x05,                               // syscall
    };
    CHECK(mem.write(0x400000, main_code.data(), main_code.size()));
    const auto wa = worker_code(static_cast<std::uint32_t>(kCa),
                                static_cast<std::uint32_t>(kCb));
    const auto wb = worker_code(static_cast<std::uint32_t>(kCb),
                                static_cast<std::uint32_t>(kCa));
    CHECK(mem.write(0x400100, wa.data(), wa.size()));
    CHECK(mem.write(0x400200, wb.data(), wb.size()));
    write_param(mem, kData + 0x100, 0, 0x300000, 0x8000, 0x400100);
    write_param(mem, kData + 0x140, 0, 0x310000, 0x8000, 0x400200);

    k.spawn_thread("main", 0x400000, 0, kMainStackTop);
    const int rc = k.run_guest(500'000);
    CHECK(rc == 7);
    CHECK(!k.had_fault());
    std::uint32_t ca = 0, cb = 0, done = 0;
    CHECK(mem.read_value(kCa, ca));
    CHECK(mem.read_value(kCb, cb));
    CHECK(mem.read_value(kDone, done));
    CHECK(ca == 25000); // both workers ran to completion
    CHECK(cb == 25000);
    CHECK(done == 2);
    CHECK(k.threads().size() == 3);
    CHECK(k.enosys_count() == 0);
}

} // namespace

int main() {
    test_getpid_and_spawn();
    test_unknown_syscall_is_enosys();
    test_anon_mmap_round_trip();
    test_open_write_lseek_read();
    test_tty_capture_and_gettimeofday();
    test_identity_and_ppid();
    test_clock_gettime_and_nanosleep();
    test_ioctl_enotty();
    test_sysctl();
    test_stat_family();
    test_file_backed_mmap();
    test_guest_program_syscalls_end_to_end();
    test_multithreaded_guest_futex();
    test_scheduler_sleep();
    test_futex_deadlock_detected();
    test_interleaved_cpu_bound_workers();
    if (g_failures == 0) std::cout << "test_kernel: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
