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

void test_adc_sbb_carry() {
    Machine m;
    // stc ; mov rax,5 ; mov rbx,7 ; adc rax,rbx ; clc ; sbb rax,rbx ;
    // stc ; mov rax,-1 ; adc rax,1 ; hlt
    m.setup({
        0xF9,                                     // stc
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00, // mov rax,5
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00, // mov rbx,7
        0x48, 0x11, 0xD8,                         // adc rax,rbx -> 13
        0xF8,                                     // clc
        0x48, 0x19, 0xD8,                         // sbb rax,rbx -> 6
        0xF9,                                     // stc
        0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, // mov rax,-1
        0x48, 0x83, 0xD0, 0x00,                   // adc rax,0 -> -1+0+1 = 0, CF=1
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 0);
    CHECK((m.cpu.rflags & kura::cpu::kCF) != 0); // carry out survived
}

void test_shifts_and_rotates() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0xF0, 0xFF, 0xFF, 0xFF, // mov rax,-16
        0x48, 0xC1, 0xE0, 0x04,                   // shl rax,4 -> ...F00
        0x48, 0xC1, 0xF8, 0x04,                   // sar rax,4 -> ...FF0
        0x48, 0xD1, 0xE8,                         // shr rax,1 -> 0x7FF...F8
        0x48, 0xBB, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x80,                   // mov rbx,0x8000000000000001
        0x48, 0xD1, 0xC3,                         // rol rbx,1 -> 3
        0x48, 0xD1, 0xCB,                         // ror rbx,1 -> back
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 0x7FFFFFFFFFFFFFF8ull);
    CHECK(m.cpu.gpr[kura::cpu::RBX] == 0x8000000000000001ull);
}

void test_muldiv_family() {
    Machine m;
    // mov rax,100 ; mov rbx,7 ; mul rbx ; div rbx ;
    // mov rax,-7 ; cqo ; mov rcx,2 ; idiv rcx ; hlt
    m.setup({
        0x48, 0xC7, 0xC0, 0x64, 0x00, 0x00, 0x00, // mov rax,100
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00, // mov rbx,7
        0x48, 0xF7, 0xE3,                         // mul rbx -> 700
        0x48, 0xF7, 0xF3,                         // div rbx -> rax=100 rdx=0
        0x48, 0xC7, 0xC0, 0xF9, 0xFF, 0xFF, 0xFF, // mov rax,-7
        0x48, 0x99,                               // cqo
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx,2
        0x48, 0xF7, 0xF9,                         // idiv rcx -> -3 rem -1
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == static_cast<std::uint64_t>(-3));
    CHECK(m.cpu.gpr[kura::cpu::RDX] == static_cast<std::uint64_t>(-1));
}

void test_imul_high_word() {
    Machine m;
    // signed 64x64 whose full product needs the high word (RDX):
    // -3 * 5 = -15 => RDX:RAX = 0xFFFFFFFF_FFFFFFF1
    m.setup({
        0x48, 0xC7, 0xC0, 0xFD, 0xFF, 0xFF, 0xFF, // mov rax,-3
        0x48, 0xC7, 0xC3, 0x05, 0x00, 0x00, 0x00, // mov rbx,5
        0x48, 0xF7, 0xEB,                         // imul rbx
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 0xFFFFFFFFFFFFFFF1ull);
    CHECK(m.cpu.gpr[kura::cpu::RDX] == 0xFFFFFFFFFFFFFFFFull);
}

void test_divide_error() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00, // mov rax,5
        0x48, 0xC7, 0xC3, 0x00, 0x00, 0x00, 0x00, // mov rbx,0
        0x48, 0xF7, 0xF3,                         // div rbx -> #DE
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::DivideError);
    CHECK(!r.detail.empty());
}

void test_setcc_cmovcc() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00, // mov rax,5
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00, // mov rbx,7
        0x48, 0x39, 0xD8,                         // cmp rax,rbx (5 < 7 signed)
        0x0F, 0x9C, 0xC1,                         // setl cl -> 1
        0x48, 0x0F, 0x4C, 0xC3,                   // cmovl rax,rbx -> rax=7
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 7);
    CHECK((m.cpu.gpr[kura::cpu::RCX] & 0xFF) == 1);
}

void test_cmov_reads_memory_even_when_false() {
    Machine m;
    // CMOVcc must fault on unmapped memory even when the condition is
    // false (real x86 semantics) — otherwise loads would be reordered.
    m.setup({
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax,1
        0x48, 0x39, 0xC0,                         // cmp rax,rax -> ZF (L false)
        0x48, 0x0F, 0x4C, 0x03,                   // cmovl rax,[rbx]
    });
    m.cpu.gpr[kura::cpu::RBX] = 0x90000000ull; // unmapped
    auto r = m.interp.run(100);
    CHECK(r.reason == StopReason::DataFault);
}

void test_xchg() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00, // mov rax,42
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00, // mov rbx,7
        0x48, 0x87, 0xD8,                         // xchg rax,rbx
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 7);
    CHECK(m.cpu.gpr[kura::cpu::RBX] == 42);
}

void test_acc_imm_and_byte_alu() {
    Machine m;
    // add rax,-16 (05) ; add eax,32 (83) ; add al,5 (80) ;
    // cmp eax,37 (3D) ; je +7 ; mov rax,99 ; hlt
    m.setup({
        0x48, 0xC7, 0xC0, 0x10, 0x00, 0x00, 0x00, // mov rax,16
        0x48, 0x05, 0xF0, 0xFF, 0xFF, 0xFF,       // add rax,-16 -> 0
        0x83, 0xC0, 0x20,                         // add eax,32 -> 32
        0x80, 0xC0, 0x05,                         // add al,5 -> 37
        0x3D, 0x25, 0x00, 0x00, 0x00,             // cmp eax,37 -> ZF
        0x74, 0x07,                               // je over the mov
        0x48, 0xC7, 0xC0, 0x63, 0x00, 0x00, 0x00, // mov rax,99 (skipped)
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 37);
}

void test_imul_imm_forms() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0x06, 0x00, 0x00, 0x00, // mov rax,6
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00, // mov rbx,7
        0x48, 0x69, 0xC3, 0x07, 0x00, 0x00, 0x00, // imul rax,rbx,7 -> 49
        0x48, 0x6B, 0xC0, 0x03,                   // imul rax,rax,3 -> 147
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 147);
}

void test_not_neg() {
    Machine m;
    m.setup({
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00, // mov rax,5
        0x48, 0xF7, 0xD8,                         // neg rax -> -5
        0x48, 0xF7, 0xD0,                         // not rax -> 4
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 4);
}

void test_rep_stosq_movsq() {
    Machine m;
    m.setup({
        0x48, 0xBF, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdi,0x300000
        0x48, 0xC7, 0xC0, 0x77, 0x66, 0x55, 0x44,                   // mov rax,0x44556677
        0x48, 0xC7, 0xC1, 0x04, 0x00, 0x00, 0x00,                   // mov rcx,4
        0xF3, 0x48, 0xAB,                                           // rep stosq
        0x48, 0xBE, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rsi,0x300000
        0x48, 0xBF, 0x00, 0x08, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdi,0x300800
        0x48, 0xC7, 0xC1, 0x04, 0x00, 0x00, 0x00,                   // mov rcx,4
        0xF3, 0x48, 0xA5,                                           // rep movsq
        0x48, 0x89, 0xC8,                                           // mov rax,rcx
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RAX] == 0); // rep drained rcx
    CHECK(m.cpu.gpr[kura::cpu::RDI] == 0x300820);
    CHECK(m.cpu.gpr[kura::cpu::RSI] == 0x300020);
    std::uint64_t v = 0;
    CHECK(m.mem.read_value(0x300000, v));
    CHECK(v == 0x44556677ull);
    CHECK(m.mem.read_value(0x300800, v));
    CHECK(v == 0x44556677ull);
    CHECK(m.mem.read_value(0x300818, v));
    CHECK(v == 0x44556677ull);
}

void test_string_backward() {
    Machine m;
    m.setup({
        0x48, 0xBF, 0x18, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdi,0x300018
        0x48, 0xC7, 0xC0, 0x11, 0x22, 0x33, 0x00,                   // mov rax,0x332211
        0xFD,                                                       // std (DF=1)
        0x48, 0xAB,                                                 // stosq
        0xFC,                                                       // cld
        0xF4,
    });
    auto r = m.interp.run(1000);
    CHECK(r.reason == StopReason::Halted);
    CHECK(m.cpu.gpr[kura::cpu::RDI] == 0x300010); // moved backwards
    CHECK(!m.cpu.df);
    std::uint64_t v = 0;
    CHECK(m.mem.read_value(0x300018, v));
    CHECK(v == 0x332211ull);
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
    test_adc_sbb_carry();
    test_shifts_and_rotates();
    test_muldiv_family();
    test_imul_high_word();
    test_divide_error();
    test_setcc_cmovcc();
    test_cmov_reads_memory_even_when_false();
    test_xchg();
    test_acc_imm_and_byte_alu();
    test_imul_imm_forms();
    test_not_neg();
    test_rep_stosq_movsq();
    test_string_backward();
    if (g_failures == 0) std::cout << "test_cpu: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
