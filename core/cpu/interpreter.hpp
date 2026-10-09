#pragma once

#include <cstdint>
#include <string>

#include "memory/guest_memory.hpp"

namespace kura::cpu {

// ---- rflags bits (subset we track) ----
inline constexpr std::uint64_t kCF = 1ull << 0;
inline constexpr std::uint64_t kPF = 1ull << 2;
inline constexpr std::uint64_t kZF = 1ull << 6;
inline constexpr std::uint64_t kSF = 1ull << 7;
inline constexpr std::uint64_t kOF = 1ull << 11;
inline constexpr std::uint64_t kFlagFixed = 1ull << 1; // bit 1 is always set on x86

// GPR indices in x86-64 encoding order.
enum Gpr : int {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

struct CpuState {
    std::uint64_t gpr[16] = {};
    std::uint64_t rip = 0;
    std::uint64_t rflags = kFlagFixed;
    bool df = false; // direction flag (string ops; false = forward)
};

enum class StopReason {
    Halted,        // HLT
    Syscall,       // SYSCALL intercepted but not consumed by the host hook
    Exited,        // host consumed an exit syscall
    InvalidOpcode, // decoder hit something we don't implement (yet)
    FetchFault,    // RIP outside mapped guest memory
    DataFault,     // operand address outside mapped guest memory
    DivideError,   // DIV/IDIV by zero or quotient overflow
    StepLimit,     // max_steps exhausted (running, not an error)
};

const char* to_string(StopReason r);

struct RunResult {
    StopReason reason = StopReason::StepLimit;
    std::uint64_t steps = 0;
    int exit_code = 0;      // valid when reason == Exited
    std::string detail;     // human-readable fault info
};

// Interpreter-first CPU backend (DESIGN.md §5.4): decodes a practical
// x86-64 subset, executes against GuestMemory. Software-bound to the
// host machine — it can never execute guest code natively (§11).
class Interpreter {
public:
    // Return true to consume the syscall and let the guest continue;
    // return false to stop the run with StopReason::Syscall.
    using SyscallHook = bool (*)(CpuState&, GuestMemory&, void* user);
    // Called for exit-style syscalls (FreeBSD 1 / Linux 60). Set exit_code
    // and return true to continue, false to stop with StopReason::Exited.
    using ExitHook = bool (*)(CpuState&, int code, void* user);

    Interpreter(GuestMemory& mem, CpuState& state) : mem_(mem), st_(state) {}

    void set_syscall_hook(SyscallHook h, void* user = nullptr) {
        syscall_hook_ = h;
        syscall_user_ = user;
    }
    void set_exit_hook(ExitHook h, void* user = nullptr) {
        exit_hook_ = h;
        exit_user_ = user;
    }

    // Runs up to max_steps instructions starting at st_.rip.
    RunResult run(std::uint64_t max_steps);

    // Executes exactly one instruction; returns false when the run must stop
    // (result filled in). Used by run() and by single-step tooling.
    bool step(RunResult& out);

private:
    struct Rex {
        bool present = false;
        bool w = false, r = false, x = false, b = false;
    };
    struct RmOperand {
        bool is_reg = false;
        int reg = 0;
        std::uint64_t addr = 0;
        bool rip_rel = false;          // addr resolved as pc_ + disp after full fetch
        std::int64_t disp = 0;
    };

    // Final effective address (resolves RIP-relative forms against pc_,
    // which points past the whole instruction once decode+immediates are read).
    std::uint64_t effective(const RmOperand& op) const {
        return op.rip_rel ? static_cast<std::uint64_t>(
                                static_cast<std::int64_t>(pc_) + op.disp)
                          : op.addr;
    }

    // cursor helpers — all bounds-checked against GuestMemory
    bool fetch8(std::uint8_t& v);
    bool fetch32(std::uint32_t& v);
    bool fetch64(std::uint64_t& v);

    RmOperand decode_rm(const Rex& rex, int& reg_field);

    std::uint64_t get_reg(int i) const { return st_.gpr[i & 15]; }
    void set_reg(int i, std::uint64_t v, int width);

    bool read_rm(const RmOperand& op, int width, std::uint64_t& out);
    bool write_rm(const RmOperand& op, int width, std::uint64_t v);

    // raw little-endian access at a guest address (string ops, rep helpers)
    bool mem_read(std::uint64_t addr, int bytes, std::uint64_t& out);
    bool mem_write(std::uint64_t addr, int bytes, std::uint64_t v);

    bool eval_cc(unsigned nibble) const;
    void set_zsp(std::uint64_t res, int width);

    bool alu_op(unsigned digit, std::uint64_t a, std::uint64_t b, int width,
                std::uint64_t& result);

    // shift/rotate group (digits: 0 ROL, 1 ROR, 4 SHL, 5 SHR, 7 SAR)
    void shift_op(int digit, std::uint64_t a, int width, std::uint64_t count,
                  std::uint64_t& res);

    // F6/F7 group /4-/7: MUL, IMUL, DIV, IDIV (one-operand forms).
    // false + filled out => stop the run (DivideError or DataFault).
    bool exec_muldiv(unsigned digit, const RmOperand& rm, int width, RunResult& out);

    GuestMemory& mem_;
    CpuState& st_;
    SyscallHook syscall_hook_ = nullptr;
    void* syscall_user_ = nullptr;
    ExitHook exit_hook_ = nullptr;
    void* exit_user_ = nullptr;

    std::uint64_t pc_ = 0;      // decode cursor for the current instruction
    bool fault_ = false;        // set by fetch*/read_rm on bounds failure
    std::string fault_detail_;
};

} // namespace kura::cpu
