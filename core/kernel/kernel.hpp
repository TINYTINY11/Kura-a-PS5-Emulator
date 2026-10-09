#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cpu/interpreter.hpp"
#include "kernel/dynlib.hpp"
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

// Scheduler quantum: instructions per thread slice before the round-robin
// rotates (cooperative slicing — 20k is ~sub-millisecond of interpreter
// work, fine-grained enough that a busy loop can't starve its siblings).
inline constexpr std::uint64_t kQuantum = 20'000;

// A guest thread (M3 stage 6). Cooperative green threads: each thread
// owns its register state and a dedicated interpreter over the SHARED
// GuestMemory; the scheduler runs one thread per quantum, so guest
// parallelism never becomes host data races (§11 — deterministic and
// race-free by construction). Blocking syscalls (futex wait, nanosleep,
// thr_exit) end the quantum early and the thread is resumed or retired
// by the scheduler.
struct Thread {
    std::uint64_t tid = 0;
    std::uint64_t tgid = 0; // thread-group id == owning process pid
    std::string name;
    enum class Status { Ready, Running, Sleeping, FutexWait, Exited };
    Status status = Status::Ready;
    int exit_code = 0;
    std::int64_t wake_ns = 0;      // steady-clock deadline (Sleeping)
    std::uint64_t futex_addr = 0;  // wait word (FutexWait)
    cpu::CpuState state;           // live register file while scheduled
    std::unique_ptr<cpu::Interpreter> interp; // references `state` — heap
    // owned, address-stable (vector of unique_ptr keeps refs valid)
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

    // --- M3 stage 6: cooperative scheduler ---------------------------------
    // Spawns a thread (tid auto, tgid = current process) at `entry` with
    // RDI=arg and RSP=stack_top. Registers its own interpreter over the
    // shared GuestMemory.
    Thread* spawn_thread(const std::string& name, std::uint64_t entry,
                         std::uint64_t arg, std::uint64_t stack_top);
    // Runs all threads round-robin until the process exits, every thread
    // retires, a guest fault/deadlock occurs, or max_quanta is hit
    // (watchdog — the scheduler can never hang the host). Returns the
    // process exit code; had_fault() reports faults/deadlock/watchdog.
    int run_guest(std::uint64_t max_quanta = 1'000'000);
    bool had_fault() const { return had_fault_; }
    const std::vector<std::unique_ptr<Thread>>& threads() const {
        return threads_;
    }
    Thread* find_thread(std::uint64_t tid);

    Vfs& vfs() { return vfs_; }
    const Vfs& vfs() const { return vfs_; }

    // Module registry (M3 stage 8): dynlib machinery with syscall numbers
    // pending real-binary verification (see dynlib.hpp).
    Dynlib& dynlib() { return dynlib_; }
    const Dynlib& dynlib() const { return dynlib_; }

    const Process* current() const;
    bool has_exited() const;
    int exit_code() const;

    // Base address handed out by the next anonymous mmap (a bump allocator;
    // munmap'd holes get recycled in a later stage).
    std::uint64_t heap_base() const { return heap_next_; }

    // Syscall trace (M3 stage 7): counts every dispatched syscall and every
    // ENOSYS — "trace is clean" means enosys_count() == 0 for a program.
    std::uint64_t syscall_count() const { return syscall_count_; }
    std::uint64_t enosys_count() const { return enosys_count_; }

private:
    static bool syscall_trampoline(cpu::CpuState& st, GuestMemory& mem, void* user);
    static bool exit_trampoline(cpu::CpuState& st, int code, void* user);

    // Why the kernel stopped a quantum early (consumed by run_guest).
    enum class StopKind { None, Blocked, ThreadExit };

    // >= 0 on success, < 0 = -errno, dispatch logs unknown numbers.
    std::int64_t dispatch(std::uint64_t nr, cpu::CpuState& st);
    Process* find(std::uint64_t pid);
    bool read_path(std::uint64_t addr, std::string& out);
    StopKind take_stop();

    GuestMemory& mem_;
    Vfs vfs_;
    Dynlib dynlib_;
    std::vector<Process> procs_;
    std::uint64_t current_pid_ = 0;
    std::uint64_t next_pid_ = 1;
    std::uint64_t heap_next_ = 0x2000000000ull;

    // scheduler state
    std::vector<std::unique_ptr<Thread>> threads_;
    std::uint64_t next_tid_ = 1;
    std::uint64_t running_tid_ = 0;
    bool scheduling_ = false;
    StopKind pending_stop_ = StopKind::None;
    bool had_fault_ = false;
    std::uint64_t syscall_count_ = 0;
    std::uint64_t enosys_count_ = 0;
};

} // namespace kura::kernel
