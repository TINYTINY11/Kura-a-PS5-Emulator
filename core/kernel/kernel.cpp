#include "kernel/kernel.hpp"

#include <algorithm>
#include <chrono>

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
        case kLinuxExit: return "exit (linux-style)";
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
    const std::uint64_t a3 = arg(st, 3), a4 = arg(st, 4);

    switch (nr) {
    case kGetpid:
        return static_cast<std::int64_t>(current_pid_);

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

    default:
        log::debug("kernel.sys", "syscall ", nr, " (", name(nr),
                   ") not implemented — returning ENOSYS");
        return -kENOSYS;
    }
}

} // namespace kura::kernel
