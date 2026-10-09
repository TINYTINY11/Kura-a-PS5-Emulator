#include "cpu/interpreter.hpp"

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

using kura::GuestMemory;
using kura::cpu::CpuState;
using kura::cpu::Interpreter;
using kura::cpu::RunResult;
using kura::cpu::StopReason;

struct Machine {
    GuestMemory mem;
    CpuState cpu;
    Interpreter interp{mem, cpu};

    // code at 0x400000, data at 0x300000, stack top at 0x80000000
    void setup(const std::vector<std::uint8_t>& code) {
        mem.map(0x400000, 0x10000);
        mem.map(0x300000, 0x1000);
        mem.map(0x7FFF0000, 0x10000);
        mem.write(0x400000, code.data(), code.size());
        cpu.rip = 0x400000;
        cpu.gpr[kura::cpu::RSP] = 0x80000000ull;
    }
};

void test_arithmetic() {
    Machine m;
    // mov rax,5 ; mov rbx,7 ; add rax,rbx ; hlt
    m.setup({
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xD8,
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 12);
}

void test_loop_sum() {
    Machine m;
    // ecx=0; edx=10; loop: ecx+=edx; edx--; cmp edx,0; jnz loop; hlt
    m.setup({
        0xB9, 0x00, 0x00, 0x00, 0x00,       // mov ecx, 0
        0xBA, 0x0A, 0x00, 0x00, 0x00,       // mov edx, 10
        0x48, 0x01, 0xD1,                   // +0:  add rcx, rdx
        0x48, 0xFF, 0xCA,                   //      dec rdx
        0x48, 0x83, 0xFA, 0x00,             //      cmp rdx, 0
        0x75, 0xF4,                         //      jnz -12 -> +0
        0xF4,                               //      hlt
    });
    auto r = m.interp.run(10000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RCX] == 55);  // 10+9+...+1
    CHECK(m.cpu.gpr[kura::cpu::RDX] == 0);
}

void test_memory_store_load() {
    Machine m;
    // mov rbx,0x300000 ; mov rax,0x55667788 ; mov [rbx+8],rax ; mov rcx,[rbx+8] ; hlt
    m.setup({
        0x48, 0xC7, 0xC3, 0x00, 0x00, 0x30, 0x00,
        0x48, 0xC7, 0xC0, 0x88, 0x77, 0x66, 0x55,
        0x48, 0x89, 0x43, 0x08,
        0x48, 0x8B, 0x4B, 0x08,
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RCX] == 0x55667788ull);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 0x55667788ull);
}

void test_call_ret_stack() {
    Machine m;
    // call target ; mov rax,42 ; hlt ; target: mov rax,7 ; ret
    m.setup({
        0xE8, 0x08, 0x00, 0x00, 0x00,             // call +8 -> 0x40000D
        0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00, // mov rax, 42
        0xF4,                                      // hlt
        0x48, 0xC7, 0xC0, 0x07, 0x00, 0x00, 0x00, // target: mov rax, 7
        0xC3,                                      // ret
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 42);      // callee ran, then returned
    CHECK(m.cpu.gpr[kura::cpu::RSP] == 0x80000000ull); // stack balanced
}

void test_push_pop() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00, // mov rax, 42
        0x50,                                       // push rax
        0x48, 0x31, 0xDB,                           // xor rbx, rbx
        0x5B,                                       // pop rbx
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RBX] == 42);
    CHECK(m.cpu.gpr[kura::cpu::RSP] == 0x80000000ull);
}

void test_je_taken() {
    Machine m;
    // mov rax,5 ; cmp rax,5 ; je skip ; mov rax,99 ; skip: hlt
    m.setup({
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,
        0x48, 0x83, 0xF8, 0x05,
        0x74, 0x07,                         // je -> hlt
        0x48, 0xC7, 0xC0, 0x63, 0x00, 0x00, 0x00,
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 5); // not overwritten with 99
}

void test_exit_syscall() {
    Machine m;
    // mov rax,1 ; mov rdi,42 ; syscall   (FreeBSD SYS_exit)
    m.setup({
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,
        0x48, 0xC7, 0xC7, 0x2A, 0x00, 0x00, 0x00,
        0x0F, 0x05,
    });
    static int captured = -1;
    m.interp.set_exit_hook(
        [](CpuState&, int code, void* user) {
            *static_cast<int*>(user) = code;
            return false; // stop the run
        },
        &captured);
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Exited);
    CHECK(r.exit_code == 42);
    CHECK(captured == 42);
}

void test_syscall_hook_consumes() {
    Machine m;
    // mov rax,20 ; syscall ; hlt
    m.setup({
        0x48, 0xC7, 0xC0, 0x14, 0x00, 0x00, 0x00,
        0x0F, 0x05,
        0xF4,
    });
    static int calls = 0;
    m.interp.set_syscall_hook(
        [](CpuState& st, GuestMemory&, void*) -> bool {
            ++calls;
            st.gpr[kura::cpu::RAX] = 7; // fake return value
            return true;                // handled -> continue guest
        });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(calls == 1);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 7);
}

void test_unhandled_syscall_stops() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0x14, 0x00, 0x00, 0x00,
        0x0F, 0x05,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Syscall);
}

void test_movzx_and_lea_rip() {
    Machine m;
    // lea rax,[rip+0] ; mov rbx,0x1FFFFFFFF ; movzx rcx,bl ; hlt
    m.setup({
        0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00, // rax = 0x400007
        0x48, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00,
        0x48, 0x0F, 0xB6, 0xCB,                    // movzx rcx, bl
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 0x400007);
    CHECK(m.cpu.gpr[kura::cpu::RCX] == 0xFF);
}

void test_faults() {
    { // unmapped fetch
        Machine m;
        m.setup({0xF4});
        m.cpu.rip = 0xDEAD0000ull;
        auto r = m.interp.run(100);
        CHECK(r.reason == StopReason::FetchFault);
    }
    { // unmapped load
        Machine m;
        // mov rax,[rbx] with rbx unmapped
        m.setup({0x48, 0x8B, 0x03});
        m.cpu.gpr[kura::cpu::RBX] = 0x90000000ull;
        auto r = m.interp.run(100);
        CHECK(r.reason == StopReason::DataFault);
    }
    { // invalid opcode
        Machine m;
        m.setup({0x06}); // PUSH ES — invalid in long mode
        auto r = m.interp.run(100);
        CHECK(r.reason == StopReason::InvalidOpcode);
        CHECK(!r.detail.empty());
    }
    { // step limit keeps running, doesn't fake success
        Machine m;
        m.setup({0xEB, 0xFE}); // jmp self
        auto r = m.interp.run(500);
        CHECK(r.reason == StopReason::StepLimit);
        CHECK(r.steps == 500);
    }
}

} // namespace

int main() {
    test_arithmetic();
    test_loop_sum();
    test_memory_store_load();
    test_call_ret_stack();
    test_push_pop();
    test_je_taken();
    test_exit_syscall();
    test_syscall_hook_consumes();
    test_unhandled_syscall_stops();
    test_movzx_and_lea_rip();
    test_faults();
    if (g_failures == 0) std::cout << "test_cpu: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
