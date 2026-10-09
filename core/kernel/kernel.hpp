#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpu/interpreter.hpp"
#include "kernel/vfs.hpp"
#include "memory/guest_memory.hpp"

namespace kura::kernel {

// A guest process, HLE-style (design doc §5.3). Stage 1 tracks identity
// and exit state; fds live in the shared Vfs for now, resource limits and
// full proc structures come with deeper M3 work.
struct Process {
    std::uint64_t pid = 0;
    std::uint64_t ppid = 0;
    std::string name;
    bool exited = false;
    int exit_code = 0;
};

// M3 stage 1 kernel HLE: syscall dispatch + process bookkeeping + the
// beginnings of a virtual file system. FreeBSD-style convention: syscall
// result goes straight into RAX (negated errno on error).
//
// The kernel never touches the host beyond its own memory (§11): guest
// pointers are bounds-checked through GuestMemory, guest paths resolve in
// the in-memory Vfs namespace, and time comes from the host clock read-only.
class Kernel {
public:
    // The kernel starts with an "init" process (pid 1) selected — enough
    // for single-process early boot.
    explicit Kernel(GuestMemory& mem);

    // Hooks the kernel into an interpreter (syscall + exit hooks).
    void install(cpu::Interpreter& interp);

    // Adds a process and selects it. pid 0 = auto-assign.
    Process& spawn_process(const std::string& name, std::uint64_t pid = 0);

    // Direct entry points (used by the interpreter hooks and by tests).
    bool on_syscall(cpu::CpuState& st);          // continue the guest
    bool on_exit(cpu::CpuState& st, int code);   // false => stop the run

    Vfs& vfs() { return vfs_; }
    const Vfs& vfs() const { return vfs_; }

    const Process* current() const;
    bool has_exited() const;
    int exit_code() const;

    // Base address handed out by the next anonymous mmap (a bump allocator;
    // munmap'd holes get recycled in a later stage).
    std::uint64_t heap_base() const { return heap_next_; }

private:
    static bool syscall_trampoline(cpu::CpuState& st, GuestMemory& mem, void* user);
    static bool exit_trampoline(cpu::CpuState& st, int code, void* user);

    // >= 0 on success, < 0 = -errno, dispatch logs unknown numbers.
    std::int64_t dispatch(std::uint64_t nr, cpu::CpuState& st);
    Process* find(std::uint64_t pid);
    bool read_path(std::uint64_t addr, std::string& out);

    GuestMemory& mem_;
    Vfs vfs_;
    std::vector<Process> procs_;
    std::uint64_t current_pid_ = 0;
    std::uint64_t next_pid_ = 1;
    std::uint64_t heap_next_ = 0x2000000000ull;
};

} // namespace kura::kernel
