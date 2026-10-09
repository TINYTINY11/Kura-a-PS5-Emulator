#include "kernel/kernel.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "common/log.hpp"
#include "kernel/syscalls.hpp"

namespace kura::kernel {

const char* sys::name(std::uint64_t nr) {
    switch (nr) {
        case kExit: return "exit";
        case kRead: return "read";
        case kWrite: return "write";
        case kOpen: return "open";
        case kClose: return "close";
        case kGetpid: return "getpid";
        case kMunmap: return "munmap";
        case kMprotect: return "mprotect";
        case kGettimeofday: return "gettimeofday";
        case kSysctl: return "sysctl";
        case kMmap: return "mmap";
        case kLseek: return "lseek";
        case kIoctl: return "ioctl";
        case kNanosleep: return "nanosleep";
        case kGetuid: return "getuid";
        case kGeteuid: return "geteuid";
        case kGetgid: return "getgid";
        case kGetegid: return "getegid";
        case kGetppid: return "getppid";
        case kClockGettime: return "clock_gettime";
        case kStat: return "stat";
        case kFstat: return "fstat";
        case kLstat: return "lstat";
        default: return "unknown";
    }
}

namespace {

// mmap(2) flag values (FreeBSD)
constexpr std::uint64_t kMapShared = 0x0001;
constexpr std::uint64_t kMapPrivate = 0x0002;
constexpr std::uint64_t kMapFixed = 0x0010;
constexpr std::uint64_t kMapAnon = 0x1000;

constexpr std::uint64_t kPageSize = 0x1000;
std::uint64_t round_up_page(std::uint64_t v) {
    return (v + kPageSize - 1) & ~(kPageSize - 1);
}

std::uint64_t arg(const cpu::CpuState& st, int i) {
    // amd64 syscall argument registers, in order.
    static constexpr int kOrder[] = {cpu::RDI, cpu::RSI, cpu::RDX, cpu::R10,
                                     cpu::R8, cpu::R9};
    return st.gpr[kOrder[i % 6]];
}

// struct stat for FreeBSD amd64 — PREDICTED layout, 160 bytes total:
//   dev 0, ino 8, mode 16, nlink 20, uid 24, gid 28, rdev 32,
//   atim 40, mtim 56, ctim 72, birthtim 88 (each timespec 16 bytes),
//   size 104, blocks 112, blksize 120, flags 124, gen 128
// Type sizes assumed: dev/ino/off/blkcnt 8, mode/nlink/uid/gid/blksize/
// flags 4. Every offset gets a verification pass against real binaries
// when the decryption wall falls (docs/RE-pup.md).
constexpr std::uint64_t kStatSize = 160;

bool write_stat(GuestMemory& mem, std::uint64_t at,
                const Vfs::StatInfo& si) {
    std::uint8_t b[kStatSize] = {};
    const auto put64 = [&](int off, std::uint64_t v) {
        for (int i = 0; i < 8; ++i)
            b[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    const auto put32 = [&](int off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };

    const std::uint32_t type =
        si.is_char ? kS_IFCHR : (si.is_dir ? kS_IFDIR : kS_IFREG);
    put64(8, si.ino);
    put32(16, type | (si.mode_perm & 0777));
    put32(20, 1); // nlink — the flat store has no hard links
    // uid/gid stay 0 (matches the root-placeholder identity syscalls)
    // All four timestamps carry mtime — Vfs tracks one moment per file;
    // atime/ctime/birthtim granularity arrives with the host-bridge Vfs.
    put64(40, static_cast<std::uint64_t>(si.mtime_sec));
    put64(48, static_cast<std::uint64_t>(si.mtime_nsec));
    put64(56, static_cast<std::uint64_t>(si.mtime_sec));
    put64(64, static_cast<std::uint64_t>(si.mtime_nsec));
    put64(72, static_cast<std::uint64_t>(si.mtime_sec));
    put64(80, static_cast<std::uint64_t>(si.mtime_nsec));
    put64(88, static_cast<std::uint64_t>(si.mtime_sec));
    put64(96, static_cast<std::uint64_t>(si.mtime_nsec));
    put64(104, si.size);
    put64(112, (si.size + 511) / 512); // st_blocks in 512-byte units
    put32(120, 4096);                  // st_blksize
    put32(124, 0);                     // st_flags — fflags unimplemented
    return mem.write(at, b, sizeof(b));
}

} // namespace

Kernel::Kernel(GuestMemory& mem) : mem_(mem) {
    Process init;
    init.pid = next_pid_++;
    init.name = "kura-init";
    procs_.push_back(std::move(init));
    current_pid_ = procs_.front().pid;
}

void Kernel::install(cpu::Interpreter& interp) {
    interp.set_syscall_hook(&Kernel::syscall_trampoline, this);
    interp.set_exit_hook(&Kernel::exit_trampoline, this);
}

Process& Kernel::spawn_process(const std::string& name, std::uint64_t pid) {
    Process p;
    p.pid = pid ? pid : next_pid_;
    p.ppid = current_pid_;
    p.name = name;
    next_pid_ = std::max(next_pid_, p.pid + 1);
    procs_.push_back(std::move(p));
    current_pid_ = procs_.back().pid;
    return procs_.back();
}

Process* Kernel::find(std::uint64_t pid) {
    for (auto& p : procs_)
        if (p.pid == pid) return &p;
    return nullptr;
}

const Process* Kernel::current() const {
    for (const auto& p : procs_)
        if (p.pid == current_pid_) return &p;
    return nullptr;
}

bool Kernel::has_exited() const {
    const Process* p = current();
    return p != nullptr && p->exited;
}

int Kernel::exit_code() const {
    const Process* p = current();
    return p ? p->exit_code : 0;
}

bool Kernel::syscall_trampoline(cpu::CpuState& st, GuestMemory&, void* user) {
    return static_cast<Kernel*>(user)->on_syscall(st);
}

bool Kernel::exit_trampoline(cpu::CpuState& st, int code, void* user) {
    return static_cast<Kernel*>(user)->on_exit(st, code);
}

bool Kernel::on_syscall(cpu::CpuState& st) {
    const std::uint64_t nr = st.gpr[cpu::RAX];
    const std::int64_t rv = dispatch(nr, st);
    st.gpr[cpu::RAX] = static_cast<std::uint64_t>(rv);
    return true; // guest continues with the result in RAX
}

bool Kernel::on_exit(cpu::CpuState&, int code) {
    if (Process* p = find(current_pid_)) {
        p->exited = true;
        p->exit_code = code;
    }
    log::info("kernel.sys", "process ", current_pid_, " exited with code ", code);
    return false; // stop the run — the interpreter reports Exited
}

bool Kernel::read_path(std::uint64_t addr, std::string& out) {
    out.clear();
    if (addr == 0) return false;
    for (std::uint64_t i = 0; i < 1024; ++i) {
        std::uint8_t c = 0;
        if (!mem_.read_value(addr + i, c)) return false;
        if (c == 0) return true;
        out.push_back(static_cast<char>(c));
    }
    return false; // longer than any sane path; caller maps it to an error
}

std::int64_t Kernel::dispatch(std::uint64_t nr, cpu::CpuState& st) {
    using namespace sys;
    const std::uint64_t a0 = arg(st, 0), a1 = arg(st, 1), a2 = arg(st, 2);
    const std::uint64_t a3 = arg(st, 3), a4 = arg(st, 4), a5 = arg(st, 5);

    switch (nr) {
    case kGetpid:
        return static_cast<std::int64_t>(current_pid_);

    case kGetuid:
    case kGeteuid:
    case kGetgid:
    case kGetegid:
        // Root for now. The PS5 runs games as an unprivileged user; the
        // real uid/gid map needs real binaries to confirm (predicted 0).
        return 0;

    case kGetppid: {
        const Process* p = current();
        return p ? static_cast<std::int64_t>(p->ppid) : 0;
    }

    case kWrite:
        return vfs_.write(static_cast<int>(a0), mem_, a1, a2);

    case kRead:
        return vfs_.read(static_cast<int>(a0), mem_, a1, a2);

    case kOpen: {
        std::string path;
        if (!read_path(a0, path)) return -kEFAULT;
        if (path.size() >= 1024) return -kENAMETOOLONG;
        return vfs_.open(path, static_cast<std::uint32_t>(a1));
    }

    case kClose:
        return vfs_.close(static_cast<int>(a0));

    case kLseek:
        // FreeBSD amd64: lseek(fd, pad, offset, whence) — the pad keeps
        // the 64-bit offset in RDX and whence in R10.
        return vfs_.lseek(static_cast<int>(a0), static_cast<std::int64_t>(a2),
                          static_cast<int>(a3));

    case kGettimeofday: {
        if (a0 == 0) return -kEFAULT;
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const std::int64_t sec =
            std::chrono::duration_cast<std::chrono::seconds>(now).count();
        const std::int64_t usec =
            std::chrono::duration_cast<std::chrono::microseconds>(now).count() -
            sec * 1000000;
        struct timeval {
            std::int64_t tv_sec, tv_usec;
        } tv{sec, usec};
        if (!mem_.write_value(a0, tv)) return -kEFAULT;
        return 0; // tz argument deprecated in FreeBSD — ignored
    }

    case kMmap: {
        // Predicted layout pending real-binary verification: addr, len,
        // prot, flags, fd, offset. Prot is recorded but unenforced by the
        // interpreter (single flat space, M2); enforcement waits for paging.
        const std::uint64_t len = a1;
        if (len == 0) return -kEINVAL;
        const std::uint64_t size = round_up_page(len);
        const bool fixed = (a3 & kMapFixed) != 0;
        const bool anon = (a3 & kMapAnon) != 0;
        if (!anon && static_cast<int>(a4) >= 0) {
            // file-backed mappings arrive with the host-bridge Vfs stage
            log::debug("kernel.sys", "mmap: file-backed mapping not yet "
                                     "supported (fd ", static_cast<int>(a4), ")");
            return -kENOSYS;
        }
        std::uint64_t at = 0;
        if (fixed) {
            if ((a0 & (kPageSize - 1)) != 0) return -kEINVAL;
            at = a0;
        } else {
            at = heap_next_;
        }
        if (!mem_.map(at, size)) {
            log::warn("kernel.sys", "mmap: no room at 0x", at, " for ", size, " bytes");
            return -kENOMEM;
        }
        if (!fixed) heap_next_ += size + kPageSize; // guard gap between heaps
        log::debug("kernel.sys", "mmap ", size, " bytes (",
                   (fixed ? "fixed" : "heap"), ") -> 0x", std::hex, at, std::dec);
        return static_cast<std::int64_t>(at);
    }

    case kMunmap:
        if ((a0 & (kPageSize - 1)) != 0 || a1 == 0) return -kEINVAL;
        if (!mem_.unmap(a0, round_up_page(a1))) return -kEINVAL;
        log::debug("kernel.sys", "munmap 0x", std::hex, a0, std::dec,
                   " (", a1, " bytes)");
        return 0;

    case kMprotect:
        // No protection bits are enforced by the interpreter today; claim
        // success so binaries proceed. Revisit with paging.
        return 0;

    case kClockGettime: {
        // FreeBSD amd64: struct timespec { int64 tv_sec; int64 tv_nsec; }
        if (a1 == 0) return -kEFAULT;
        std::int64_t sec = 0, nsec = 0;
        if (a0 == 0) { // CLOCK_REALTIME
            const auto now =
                std::chrono::system_clock::now().time_since_epoch();
            sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
            nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                       .count() -
                   sec * 1000000000;
        } else if (a0 == 4) { // CLOCK_MONOTONIC (FreeBSD numbering)
            const auto now =
                std::chrono::steady_clock::now().time_since_epoch();
            sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
            nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                       .count() -
                   sec * 1000000000; // "since Kura start" — guest boot
        } else {
            return -kEINVAL;
        }
        if (!mem_.write_value(a1, sec) || !mem_.write_value(a1 + 8, nsec))
            return -kEFAULT;
        return 0;
    }

    case kNanosleep: {
        if (a0 == 0) return -kEFAULT;
        std::int64_t sec = 0, nsec = 0;
        if (!mem_.read_value(a0, sec) || !mem_.read_value(a0 + 8, nsec))
            return -kEFAULT;
        if (sec < 0 || nsec < 0 || nsec >= 1000000000) return -kEINVAL;
        auto d = std::chrono::seconds(sec) + std::chrono::nanoseconds(nsec);
        // Clamp: boot code sleeps in milliseconds — a mis-set guest timer
        // must never freeze the window for minutes (documented deviation).
        constexpr auto kMax = std::chrono::milliseconds(250);
        if (d > kMax) {
            log::debug("kernel.sys", "nanosleep clamped to 250ms");
            d = kMax;
        }
        std::this_thread::sleep_for(d);
        return 0;
    }

    case kIoctl:
        // Predicted number 54. FreeBSD's non-tty answer for every request
        // until the termios/winsize shapes are confirmed against real
        // binaries — isatty()-style probes treat ENOTTY cleanly.
        if (!vfs_.has_fd(static_cast<int>(a0))) return -kEBADF;
        return -kENOTTY;

    case kStat:
    case kLstat: {
        // The stage-3 Vfs has no symlinks, so lstat == stat for it.
        std::string path;
        if (!read_path(a0, path)) return -kEFAULT;
        Vfs::StatInfo si;
        if (!vfs_.stat_path(path, si)) return -kENOENT;
        if (!write_stat(mem_, a1, si)) return -kEFAULT;
        return 0;
    }

    case kFstat: {
        Vfs::StatInfo si;
        if (!vfs_.stat_fd(static_cast<int>(a0), si)) return -kEBADF;
        if (!write_stat(mem_, a1, si)) return -kEFAULT;
        return 0;
    }

    case kSysctl: {
        // sysctl(2), FreeBSD amd64: int *name, u_int namelen, void *oldp,
        // size_t *oldlenp, void *newp, size_t newlen. Kura's tree is
        // read-only; all numbers below are predicted from FreeBSD.
        if (a4 != 0 || a5 != 0) return -kEPERM; // writes rejected
        if (a1 == 0 || a1 > 16) return -kEINVAL;
        if (a3 == 0) return -kEFAULT; // oldlenp is mandatory
        int mib[16] = {};
        for (std::uint64_t i = 0; i < a1; ++i)
            if (!mem_.read_value(a0 + i * 4, mib[i])) return -kEFAULT;

        const bool kern = mib[0] == 1; // CTL_KERN
        const bool hw = mib[0] == 6;   // CTL_HW
        const int leaf = (a1 > 1) ? mib[1] : -1;

        enum class Kind { Str, Int } kind = Kind::Int;
        std::string s;
        std::int32_t v = 0;
        if (kern && a1 == 2 && leaf == 2) { // KERN_OSRELEASE
            kind = Kind::Str;
            s = "11.0-Kura"; // predicted shape of kern.osrelease
        } else if (kern && a1 == 2 && leaf == 1) { // KERN_ARG_MAX
            v = 262144;
        } else if (hw && a1 == 2 && leaf == 7) { // HW_PAGESIZE
            v = 4096;
        } else if (hw && a1 == 2 && leaf == 3) { // HW_NCPU
            v = 8; // predicted: PS5 core count exposed to the OS (TBD)
        } else {
            return -kENOENT;
        }

        const std::uint64_t need =
            (kind == Kind::Str) ? s.size() + 1 : sizeof(std::int32_t);
        std::uint64_t have = 0;
        if (!mem_.read_value(a3, have)) return -kEFAULT;
        if (a2 == 0) // size probe: report the required length
            return mem_.write_value(a3, need) ? 0 : -kEFAULT;
        if (have < need) {
            mem_.write_value(a3, need); // FreeBSD updates the length out
            return -kENOMEM;            // (predicted error for short buffer)
        }
        const bool wrote = (kind == Kind::Str)
                               ? mem_.write(a2, s.c_str(), need)
                               : mem_.write_value(a2, v);
        if (!wrote) return -kEFAULT;
        return mem_.write_value(a3, need) ? 0 : -kEFAULT;
    }

    default:
        log::debug("kernel.sys", "syscall ", nr, " (", name(nr),
                   ") not implemented — returning ENOSYS");
        return -kENOSYS;
    }
}

} // namespace kura::kernel
