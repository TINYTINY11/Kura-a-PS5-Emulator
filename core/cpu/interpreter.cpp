#include "cpu/interpreter.hpp"

#include <cstring>

namespace kura::cpu {

const char* to_string(StopReason r) {
    switch (r) {
        case StopReason::Halted: return "halted";
        case StopReason::Syscall: return "syscall";
        case StopReason::Exited: return "exited";
        case StopReason::InvalidOpcode: return "invalid opcode";
        case StopReason::FetchFault: return "fetch fault";
        case StopReason::DataFault: return "data fault";
        case StopReason::StepLimit: return "step limit";
    }
    return "unknown";
}

namespace {

constexpr int popcount8(unsigned char b) {
    int n = 0;
    for (int i = 0; i < 8; ++i) n += (b >> i) & 1;
    return n;
}

std::uint64_t mask_of(int width) {
    return width >= 64 ? ~0ull : ((1ull << width) - 1);
}

} // namespace

// ---------------------------------------------------------------- cursor ----

bool Interpreter::fetch8(std::uint8_t& v) {
    if (!mem_.read_value(pc_, v)) {
        fault_ = true;
        fault_detail_ = "fetch outside mapped memory at rip=0x" + std::to_string(pc_);
        return false;
    }
    ++pc_;
    return true;
}

bool Interpreter::fetch32(std::uint32_t& v) {
    std::uint8_t b[4];
    if (!mem_.read(pc_, b, 4)) {
        fault_ = true;
        fault_detail_ = "fetch32 outside mapped memory at rip=0x" + std::to_string(pc_);
        return false;
    }
    pc_ += 4;
    v = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
        (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
}

bool Interpreter::fetch64(std::uint64_t& v) {
    std::uint32_t lo = 0, hi = 0;
    if (!fetch32(lo) || !fetch32(hi)) return false;
    v = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return true;
}

// ------------------------------------------------------------- registers ----

void Interpreter::set_reg(int i, std::uint64_t v, int width) {
    i &= 15;
    switch (width) {
        case 8: st_.gpr[i] = (st_.gpr[i] & ~0xFFull) | (v & 0xFFull); break;
        case 16: st_.gpr[i] = (st_.gpr[i] & ~0xFFFFull) | (v & 0xFFFFull); break;
        case 32: st_.gpr[i] = v & 0xFFFFFFFFull; break; // zero-extends in long mode
        default: st_.gpr[i] = v; break;
    }
}

// --------------------------------------------------------------- decode -----

Interpreter::RmOperand Interpreter::decode_rm(const Rex& rex, int& reg_field) {
    RmOperand op{};
    std::uint8_t modrm = 0;
    if (!fetch8(modrm)) { fault_ = true; return op; }

    const unsigned mod = modrm >> 6;
    const unsigned reg = (modrm >> 3) & 7;
    const unsigned rm = modrm & 7;
    reg_field = static_cast<int>(reg | (rex.r << 3));

    if (mod == 3) {
        op.is_reg = true;
        op.reg = static_cast<int>(rm | (rex.b << 3));
        return op;
    }

    std::uint64_t addr = 0;
    bool rip_rel = false;
    std::uint64_t disp = 0;

    if (rm == 4) {
        // SIB
        std::uint8_t sib = 0;
        if (!fetch8(sib)) { fault_ = true; return op; }
        const unsigned scale = 1u << (sib >> 6);
        const unsigned index = (sib >> 3) & 7;
        const unsigned base = sib & 7;

        if (index != 4) { // index==4 (without REX.X) means "no index"
            const int idx = static_cast<int>(index | (rex.x << 3));
            addr += st_.gpr[idx] * scale;
        }
        if (base == 5 && mod == 0) {
            // no base register, disp32 only
            std::uint32_t d = 0;
            if (!fetch32(d)) { fault_ = true; return op; }
            addr += static_cast<std::int32_t>(d);
        } else {
            addr += st_.gpr[base | (rex.b << 3)];
        }
    } else if (mod == 0 && rm == 5) {
        // RIP-relative: resolved after the whole instruction is fetched
        rip_rel = true;
        std::uint32_t d = 0;
        if (!fetch32(d)) { fault_ = true; return op; }
        disp = static_cast<std::int32_t>(d);
    } else {
        addr += st_.gpr[rm | (rex.b << 3)];
    }

    if (mod == 1) {
        std::uint8_t d = 0;
        if (!fetch8(d)) { fault_ = true; return op; }
        addr += static_cast<std::int8_t>(d);
    } else if (mod == 2) {
        std::uint32_t d = 0;
        if (!fetch32(d)) { fault_ = true; return op; }
        addr += static_cast<std::int32_t>(d);
    }

    op.is_reg = false;
    op.addr = addr;
    if (rip_rel) {
        op.rip_rel = true;
        op.disp = static_cast<std::int64_t>(disp);
    }
    return op;
}

bool Interpreter::read_rm(const RmOperand& op, int width, std::uint64_t& out) {
    const int bytes = width / 8;
    if (op.is_reg) {
        out = get_reg(op.reg) & mask_of(width);
        return true;
    }
    const std::uint64_t addr = effective(op);
    std::uint8_t buf[8] = {};
    if (!mem_.read(addr, buf, static_cast<std::uint64_t>(bytes))) {
        fault_ = true;
        fault_detail_ = "load outside mapped memory at 0x" + std::to_string(addr);
        return false;
    }
    out = 0;
    for (int i = 0; i < bytes; ++i)
        out |= static_cast<std::uint64_t>(buf[i]) << (8 * i);
    return true;
}

bool Interpreter::write_rm(const RmOperand& op, int width, std::uint64_t v) {
    if (op.is_reg) {
        set_reg(op.reg, v, width);
        return true;
    }
    const int bytes = width / 8;
    const std::uint64_t addr = effective(op);
    std::uint8_t buf[8] = {};
    for (int i = 0; i < bytes; ++i)
        buf[i] = static_cast<std::uint8_t>(v >> (8 * i));
    if (!mem_.write(addr, buf, static_cast<std::uint64_t>(bytes))) {
        fault_ = true;
        fault_detail_ = "store outside mapped memory at 0x" + std::to_string(addr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- flags -----

void Interpreter::set_zsp(std::uint64_t res, int width) {
    const std::uint64_t m = mask_of(width);
    res &= m;
    if (res == 0) st_.rflags |= kZF; else st_.rflags &= ~kZF;
    if (res >> (width - 1)) st_.rflags |= kSF; else st_.rflags &= ~kSF;
    if (popcount8(static_cast<unsigned char>(res & 0xFF)) % 2 == 0)
        st_.rflags |= kPF;
    else
        st_.rflags &= ~kPF;
}

bool Interpreter::eval_cc(unsigned nibble) const {
    const std::uint64_t f = st_.rflags;
    const bool cf = (f & kCF) != 0, zf = (f & kZF) != 0, sf = (f & kSF) != 0,
               of = (f & kOF) != 0, pf = (f & kPF) != 0;
    switch (nibble & 0xF) {
        case 0x0: return of;                 // JO
        case 0x1: return !of;                // JNO
        case 0x2: return cf;                 // JB/JC
        case 0x3: return !cf;                // JAE/JNC
        case 0x4: return zf;                 // JE
        case 0x5: return !zf;                // JNE
        case 0x6: return cf || zf;           // JBE
        case 0x7: return !(cf || zf);        // JA
        case 0x8: return sf;                 // JS
        case 0x9: return !sf;                // JNS
        case 0xA: return pf;                 // JP
        case 0xB: return !pf;                // JNP
        case 0xC: return sf != of;           // JL
        case 0xD: return sf == of;           // JGE
        case 0xE: return zf || (sf != of);   // JLE
        case 0xF: return !zf && (sf == of);  // JG
    }
    return false;
}

// ------------------------------------------------------------------ ALU -----
// digit: 0 ADD, 1 OR, 2 ADC, 3 SBB, 4 AND, 5 SUB, 6 XOR, 7 CMP
bool Interpreter::alu_op(unsigned digit, std::uint64_t a, std::uint64_t b, int width,
                         std::uint64_t& result) {
    const std::uint64_t m = mask_of(width);
    a &= m;
    b &= m;
    std::uint64_t r = 0;
    bool cf = false, of = false;

    switch (digit) {
        case 0: // ADD
            if (width == 64) {
                r = a + b;
                cf = r < a;
            } else {
                const std::uint64_t t = a + b;
                cf = t > m;
                r = t & m;
            }
            of = ((~(a ^ b)) & (a ^ r) & m) >> (width - 1) & 1;
            break;
        case 5:  // SUB
        case 7:  // CMP (identical to SUB; caller discards the result)
        case 3:  // SBB (carry-in ignored for M2 subset; b already includes it if needed)
            r = (a - b) & m;
            cf = a < b;
            of = (((a ^ b) & (a ^ r)) & m) >> (width - 1) & 1;
            break;
        case 4: r = a & b; cf = of = false; break; // AND
        case 6: r = a ^ b; cf = of = false; break; // XOR
        case 1: r = a | b; cf = of = false; break; // OR
        default: return false;                     // ADC/SBB not yet
    }

    if (cf) st_.rflags |= kCF; else st_.rflags &= ~kCF;
    if (of) st_.rflags |= kOF; else st_.rflags &= ~kOF;
    set_zsp(r, width);
    result = r & m;
    return true;
}

// ------------------------------------------------------------- execute ------

RunResult Interpreter::run(std::uint64_t max_steps) {
    RunResult out;
    out.reason = StopReason::StepLimit;
    for (std::uint64_t i = 0; i < max_steps; ++i) {
        if (!step(out)) {
            out.steps = i + 1;
            return out;
        }
        out.steps = i + 1;
        out.reason = StopReason::StepLimit;
    }
    return out;
}

bool Interpreter::step(RunResult& out) {
    pc_ = st_.rip;
    fault_ = false;
    fault_detail_.clear();

    // ---- prefixes ----
    Rex rex{};
    std::uint8_t op = 0;
    for (;;) {
        if (!fetch8(op)) {
            out.reason = StopReason::FetchFault;
            out.detail = fault_detail_;
            return false;
        }
        if (op >= 0x40 && op <= 0x4F) {
            rex.present = true;
            rex.w = (op & 8) != 0;
            rex.r = (op & 4) != 0;
            rex.x = (op & 2) != 0;
            rex.b = (op & 1) != 0;
            continue;
        }
        // accepted-and-ignored legacy prefixes (M2 subset; 0x66 opsize not modeled)
        if (op == 0x66 || op == 0x67 || op == 0xF0 || op == 0xF2 || op == 0xF3 ||
            op == 0x2E || op == 0x36 || op == 0x3E || op == 0x26 || op == 0x64 ||
            op == 0x65)
            continue;
        break;
    }

    const int w = rex.w ? 64 : 32;
    int reg_field = 0;

    auto stop = [&](StopReason r, const std::string& d = "") {
        out.reason = r;
        if (!d.empty()) out.detail = d;
        return false;
    };
    auto faulted = [&]() {
        if (fault_) {
            out.reason = StopReason::DataFault;
            out.detail = fault_detail_;
            return true;
        }
        return false;
    };

    // ---- two-byte opcodes (0x0F xx) ----
    if (op == 0x0F) {
        std::uint8_t op2 = 0;
        if (!fetch8(op2)) {
            return stop(StopReason::FetchFault, fault_detail_);
        }
        if (op2 == 0x05) { // SYSCALL
            st_.rip = pc_;
            const std::uint64_t nr = st_.gpr[RAX];
            if ((nr == 1 || nr == 60) && exit_hook_) {
                const int code = static_cast<int>(st_.gpr[RDI]);
                if (!exit_hook_(st_, code, exit_user_)) {
                    out.exit_code = code;
                    return stop(StopReason::Exited);
                }
            } else if (syscall_hook_) {
                if (!syscall_hook_(st_, mem_, syscall_user_)) {
                    return stop(StopReason::Syscall,
                                "unhandled syscall " + std::to_string(nr));
                }
            } else {
                return stop(StopReason::Syscall, "syscall " + std::to_string(nr));
            }
            return true;
        }
        if (op2 >= 0x80 && op2 <= 0x8F) { // Jcc rel32
            std::uint32_t rel = 0;
            if (!fetch32(rel)) return stop(StopReason::FetchFault, fault_detail_);
            const std::int64_t srel = static_cast<std::int32_t>(rel);
            st_.rip = eval_cc(op2 & 0xF)
                          ? static_cast<std::uint64_t>(static_cast<std::int64_t>(pc_) + srel)
                          : pc_;
            return true;
        }
        if (op2 == 0x1F) { // multi-byte NOP — decode and discard
            RmOperand rm = decode_rm(rex, reg_field);
            (void)rm;
            if (faulted()) return false;
            st_.rip = pc_;
            return true;
        }
        if (op2 == 0xAF || op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t src = 0;
            int src_w = (op2 == 0xB6 || op2 == 0xBE) ? 8
                        : (op2 == 0xB7 || op2 == 0xBF) ? 16
                                                        : w;
            if (!read_rm(rm, src_w, src)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            std::uint64_t res = 0;
            if (op2 == 0xAF) { // IMUL r, r/m
                if (w == 64) {
                    res = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(get_reg(reg_field)) *
                        static_cast<std::int64_t>(src));
                } else {
                    res = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(static_cast<std::uint32_t>(get_reg(reg_field))) *
                        static_cast<std::int32_t>(static_cast<std::uint32_t>(src)));
                }
            } else if (op2 == 0xBE || op2 == 0xBF) { // MOVSX
                const std::uint64_t m = mask_of(src_w);
                const std::uint64_t sign = 1ull << (src_w - 1);
                res = (src & m) ^ sign;
                if (src & sign) res |= ~m;
                res &= mask_of(w);
            } else { // MOVZX
                res = src & mask_of(src_w);
            }
            set_reg(reg_field, res, w);
            st_.rip = pc_;
            return true;
        }
        return stop(StopReason::InvalidOpcode,
                    "unimplemented 0F " + std::to_string(op2));
    }

    // ---- one-byte opcodes ----
    switch (op) {
        case 0x90: // NOP (also covers XCHG EAX,EAX)
            st_.rip = pc_;
            return true;

        case 0xF4: // HLT
            st_.rip = pc_;
            return stop(StopReason::Halted);

        case 0x63: { // MOVSXD r64, r/m32
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t src = 0;
            if (!read_rm(rm, 32, src)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            std::uint64_t res = src;
            if (src & 0x80000000ull) res |= 0xFFFFFFFF00000000ull;
            set_reg(reg_field, res, 64);
            st_.rip = pc_;
            return true;
        }

        case 0x98: { // CBW/CWDE/CDQE — sign-extend EAX into RAX when REX.W
            const std::uint64_t a = st_.gpr[RAX];
            if (rex.w) {
                st_.gpr[RAX] = static_cast<std::int32_t>(a & 0xFFFFFFFFull);
            } else {
                st_.gpr[RAX] = (a & ~0xFFFFull) |
                               (static_cast<std::int16_t>(a & 0xFFFF) & 0xFFFF);
            }
            st_.rip = pc_;
            return true;
        }

        case 0x99: { // CDQ/CQO — sign-extend RAX into RDX
            if (rex.w)
                st_.gpr[RDX] = static_cast<std::int64_t>(st_.gpr[RAX]) < 0
                                   ? ~0ull
                                   : 0ull;
            else
                st_.gpr[RDX] = (st_.gpr[RAX] & 0x80000000ull) ? 0xFFFFFFFFull : 0ull;
            st_.rip = pc_;
            return true;
        }

        // MOV r, imm64/imm32 (0xB8+r)
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            const int r = static_cast<int>((op - 0xB8) | (rex.b << 3));
            if (rex.w) {
                std::uint64_t imm = 0;
                if (!fetch64(imm)) return stop(StopReason::FetchFault, fault_detail_);
                set_reg(r, imm, 64);
            } else {
                std::uint32_t imm = 0;
                if (!fetch32(imm)) return stop(StopReason::FetchFault, fault_detail_);
                set_reg(r, imm, 32);
            }
            st_.rip = pc_;
            return true;
        }

        // MOV r8, imm8 (0xB0+r)
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            const int r = static_cast<int>((op - 0xB0) | (rex.b << 3));
            std::uint8_t imm = 0;
            if (!fetch8(imm)) return stop(StopReason::FetchFault, fault_detail_);
            set_reg(r, imm, 8);
            st_.rip = pc_;
            return true;
        }

        // PUSH/POP r64
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: {
            const int r = static_cast<int>((op - 0x50) | (rex.b << 3));
            st_.gpr[RSP] -= 8;
            if (!mem_.write_value(st_.gpr[RSP], get_reg(r))) {
                out.reason = StopReason::DataFault;
                out.detail = "push outside mapped stack at 0x" + std::to_string(st_.gpr[RSP]);
                return false;
            }
            st_.rip = pc_;
            return true;
        }
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            const int r = static_cast<int>((op - 0x58) | (rex.b << 3));
            std::uint64_t v = 0;
            if (!mem_.read_value(st_.gpr[RSP], v)) {
                out.reason = StopReason::DataFault;
                out.detail = "pop outside mapped stack at 0x" + std::to_string(st_.gpr[RSP]);
                return false;
            }
            set_reg(r, v, 64);
            st_.gpr[RSP] += 8;
            st_.rip = pc_;
            return true;
        }

        case 0x68: { // PUSH imm32 (sign-extended)
            std::uint32_t imm = 0;
            if (!fetch32(imm)) return stop(StopReason::FetchFault, fault_detail_);
            st_.gpr[RSP] -= 8;
            const std::uint64_t v = static_cast<std::int32_t>(imm);
            if (!mem_.write_value(st_.gpr[RSP], v)) {
                out.reason = StopReason::DataFault;
                return false;
            }
            st_.rip = pc_;
            return true;
        }
        case 0x6A: { // PUSH imm8 (sign-extended)
            std::uint8_t imm = 0;
            if (!fetch8(imm)) return stop(StopReason::FetchFault, fault_detail_);
            st_.gpr[RSP] -= 8;
            const std::uint64_t v = static_cast<std::int8_t>(imm);
            if (!mem_.write_value(st_.gpr[RSP], v)) {
                out.reason = StopReason::DataFault;
                return false;
            }
            st_.rip = pc_;
            return true;
        }

        // ALU r/m, r  (01 ADD, 09 OR, 21 AND, 29 SUB, 31 XOR, 39 CMP, 85 TEST)
        case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39: case 0x85: {
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t a = 0;
            if (!read_rm(rm, w, a)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            const std::uint64_t b = get_reg(reg_field) & mask_of(w);
            std::uint64_t res = 0;
            unsigned digit = op == 0x01 ? 0 : op == 0x09 ? 1 : op == 0x21 ? 4
                             : op == 0x29 ? 5 : op == 0x31 ? 6 : op == 0x39 ? 7
                                                               : 4; // TEST -> AND-like
            if (!alu_op(digit, a, b, w, res)) return stop(StopReason::InvalidOpcode, "alu");
            if (op != 0x85 && op != 0x39) { // TEST/CMP don't write back
                if (!write_rm(rm, w, res)) {
                    out.reason = StopReason::DataFault;
                    out.detail = fault_detail_;
                    return false;
                }
            }
            st_.rip = pc_;
            return true;
        }

        // ALU r, r/m  (03 ADD, 0B OR, 23 AND, 2B SUB, 33 XOR, 3B CMP)
        case 0x03: case 0x0B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t b = 0;
            if (!read_rm(rm, w, b)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            const std::uint64_t a = get_reg(reg_field) & mask_of(w);
            std::uint64_t res = 0;
            unsigned digit = op == 0x03 ? 0 : op == 0x0B ? 1 : op == 0x23 ? 4
                             : op == 0x2B ? 5 : op == 0x33 ? 6 : 7;
            if (!alu_op(digit, a, b, w, res)) return stop(StopReason::InvalidOpcode, "alu");
            if (op != 0x3B) set_reg(reg_field, res, w);
            st_.rip = pc_;
            return true;
        }

        // TEST forms: 84 r/m8,r8 · F7 /0 r/m,imm32 · A9 EAX,imm32
        case 0x84: {
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t a = 0;
            if (!read_rm(rm, 8, a)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            std::uint64_t res = 0;
            alu_op(4, a, get_reg(reg_field) & 0xFF, 8, res); // AND-like, no writeback
            st_.rip = pc_;
            return true;
        }
        case 0xF7: {
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            if (reg_field != 0) {
                return stop(StopReason::InvalidOpcode,
                            "f7 group /" + std::to_string(reg_field) + " (not yet)");
            }
            std::uint32_t imm = 0;
            if (!fetch32(imm)) return stop(StopReason::FetchFault, fault_detail_);
            std::uint64_t a = 0;
            if (!read_rm(rm, w, a)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            std::uint64_t res = 0;
            alu_op(4, a, imm, w, res); // TEST -> AND, no writeback
            st_.rip = pc_;
            return true;
        }
        case 0xA9: { // TEST EAX, imm32
            std::uint32_t imm = 0;
            if (!fetch32(imm)) return stop(StopReason::FetchFault, fault_detail_);
            std::uint64_t res = 0;
            alu_op(4, st_.gpr[RAX] & mask_of(w), imm, w, res);
            st_.rip = pc_;
            return true;
        }

        // ALU r/m, imm  (81 imm32, 83 imm8, 84 TEST r/m8, 80 imm8 r/m8)
        case 0x81: case 0x83: {
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t imm = 0;
            if (op == 0x81) {
                std::uint32_t i32 = 0;
                if (!fetch32(i32)) return stop(StopReason::FetchFault, fault_detail_);
                imm = static_cast<std::int32_t>(i32);
            } else {
                std::uint8_t i8 = 0;
                if (!fetch8(i8)) return stop(StopReason::FetchFault, fault_detail_);
                imm = static_cast<std::int8_t>(i8);
            }
            imm &= mask_of(w);
            std::uint64_t a = 0;
            if (!read_rm(rm, w, a)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            std::uint64_t res = 0;
            if (!alu_op(reg_field, a, imm, w, res)) {
                return stop(StopReason::InvalidOpcode,
                            "alu group /" + std::to_string(reg_field));
            }
            if (reg_field != 7) { // CMP doesn't write back
                if (!write_rm(rm, w, res)) {
                    out.reason = StopReason::DataFault;
                    out.detail = fault_detail_;
                    return false;
                }
            }
            st_.rip = pc_;
            return true;
        }

        // MOV r/m, r (89) / MOV r/m8, r8 (88)
        case 0x89: case 0x88: {
            const int width = (op == 0x88 || !rex.w) ? (op == 0x88 ? 8 : 32) : 64;
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            const std::uint64_t v = get_reg(reg_field) & mask_of(width);
            if (!write_rm(rm, width, v)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            st_.rip = pc_;
            return true;
        }

        // MOV r, r/m (8B) / MOV r8, r/m8 (8A)
        case 0x8B: case 0x8A: {
            const int width = (op == 0x8A) ? 8 : (rex.w ? 64 : 32);
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t v = 0;
            if (!read_rm(rm, width, v)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            set_reg(reg_field, v, width);
            st_.rip = pc_;
            return true;
        }

        // MOV r/m, imm32 (C7) / MOV r/m8, imm8 (C6)
        case 0xC7: case 0xC6: {
            const int width = (op == 0xC6) ? 8 : (rex.w ? 64 : 32);
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            std::uint64_t imm = 0;
            if (op == 0xC6) {
                std::uint8_t i = 0;
                if (!fetch8(i)) return stop(StopReason::FetchFault, fault_detail_);
                imm = i;
            } else {
                std::uint32_t i = 0;
                if (!fetch32(i)) return stop(StopReason::FetchFault, fault_detail_);
                imm = static_cast<std::int32_t>(i); // sign-extends for 64-bit
            }
            if (reg_field != 0) {
                return stop(StopReason::InvalidOpcode,
                            "mov group /" + std::to_string(reg_field));
            }
            if (!write_rm(rm, width, imm)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            st_.rip = pc_;
            return true;
        }

        case 0x8D: { // LEA
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            if (rm.is_reg) return stop(StopReason::InvalidOpcode, "lea with register operand");
            set_reg(reg_field, effective(rm), w);
            st_.rip = pc_;
            return true;
        }

        // INC/DEC r/m (FF /0 /1), CALL r/m (FF /2), JMP r/m (FF /4), PUSH r/m (FF /6)
        case 0xFF: {
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            if (reg_field == 0 || reg_field == 1) {
                std::uint64_t a = 0;
                if (!read_rm(rm, w, a)) {
                    out.reason = StopReason::DataFault;
                    out.detail = fault_detail_;
                    return false;
                }
                const std::uint64_t old_cf = st_.rflags & kCF;
                std::uint64_t res = 0;
                if (!alu_op(reg_field == 0 ? 0u : 5u, a, reg_field == 0 ? 1u : 1u, w, res))
                    return stop(StopReason::InvalidOpcode, "inc/dec");
                st_.rflags = (st_.rflags & ~kCF) | old_cf; // INC/DEC preserve CF
                if (!write_rm(rm, w, res)) {
                    out.reason = StopReason::DataFault;
                    out.detail = fault_detail_;
                    return false;
                }
            } else if (reg_field == 2 || reg_field == 4) { // CALL/JMP r/m
                std::uint64_t target = 0;
                if (!read_rm(rm, 64, target)) {
                    out.reason = StopReason::DataFault;
                    out.detail = fault_detail_;
                    return false;
                }
                if (reg_field == 2) {
                    st_.gpr[RSP] -= 8;
                    if (!mem_.write_value(st_.gpr[RSP], pc_)) {
                        out.reason = StopReason::DataFault;
                        return false;
                    }
                }
                st_.rip = target;
                return true;
            } else if (reg_field == 6) { // PUSH r/m
                std::uint64_t v = 0;
                if (!read_rm(rm, 64, v)) {
                    out.reason = StopReason::DataFault;
                    out.detail = fault_detail_;
                    return false;
                }
                st_.gpr[RSP] -= 8;
                if (!mem_.write_value(st_.gpr[RSP], v)) {
                    out.reason = StopReason::DataFault;
                    return false;
                }
            } else {
                return stop(StopReason::InvalidOpcode,
                            "ff group /" + std::to_string(reg_field));
            }
            st_.rip = pc_;
            return true;
        }

        case 0xFE: { // INC/DEC r/m8
            RmOperand rm = decode_rm(rex, reg_field);
            if (faulted()) return false;
            if (reg_field > 1)
                return stop(StopReason::InvalidOpcode, "fe group");
            std::uint64_t a = 0;
            if (!read_rm(rm, 8, a)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            const std::uint64_t old_cf = st_.rflags & kCF;
            std::uint64_t res = 0;
            alu_op(reg_field == 0 ? 0u : 5u, a, 1, 8, res);
            st_.rflags = (st_.rflags & ~kCF) | old_cf;
            if (!write_rm(rm, 8, res)) {
                out.reason = StopReason::DataFault;
                out.detail = fault_detail_;
                return false;
            }
            st_.rip = pc_;
            return true;
        }

        // Jcc rel8 (70-7F), JMP rel8 (EB)
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
        case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0xEB: {
            std::uint8_t rel = 0;
            if (!fetch8(rel)) return stop(StopReason::FetchFault, fault_detail_);
            const std::int64_t target =
                static_cast<std::int64_t>(pc_) + static_cast<std::int8_t>(rel);
            const bool taken = (op == 0xEB) || eval_cc(op & 0xF);
            st_.rip = taken ? static_cast<std::uint64_t>(target) : pc_;
            return true;
        }

        case 0xE9: { // JMP rel32
            std::uint32_t rel = 0;
            if (!fetch32(rel)) return stop(StopReason::FetchFault, fault_detail_);
            st_.rip = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(pc_) + static_cast<std::int32_t>(rel));
            return true;
        }

        case 0xE3: { // JRCXZ rel8
            std::uint8_t rel = 0;
            if (!fetch8(rel)) return stop(StopReason::FetchFault, fault_detail_);
            st_.rip = st_.gpr[RCX] == 0
                          ? static_cast<std::uint64_t>(static_cast<std::int64_t>(pc_) +
                                                       static_cast<std::int8_t>(rel))
                          : pc_;
            return true;
        }

        case 0xE8: { // CALL rel32
            std::uint32_t rel = 0;
            if (!fetch32(rel)) return stop(StopReason::FetchFault, fault_detail_);
            st_.gpr[RSP] -= 8;
            if (!mem_.write_value(st_.gpr[RSP], pc_)) {
                out.reason = StopReason::DataFault;
                out.detail = "call push outside mapped stack";
                return false;
            }
            st_.rip = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(pc_) + static_cast<std::int32_t>(rel));
            return true;
        }

        case 0xC3: { // RET
            std::uint64_t ret = 0;
            if (!mem_.read_value(st_.gpr[RSP], ret)) {
                out.reason = StopReason::DataFault;
                out.detail = "ret outside mapped stack at 0x" + std::to_string(st_.gpr[RSP]);
                return false;
            }
            st_.gpr[RSP] += 8;
            st_.rip = ret;
            return true;
        }

        case 0xC9: { // LEAVE
            st_.gpr[RSP] = st_.gpr[RBP];
            std::uint64_t v = 0;
            if (!mem_.read_value(st_.gpr[RSP], v)) {
                out.reason = StopReason::DataFault;
                out.detail = "leave outside mapped stack";
                return false;
            }
            st_.gpr[RBP] = v;
            st_.gpr[RSP] += 8;
            st_.rip = pc_;
            return true;
        }

        default: break;
    }

    st_.rip = pc_;
    return stop(StopReason::InvalidOpcode, "opcode 0x" + std::to_string(op));
}

} // namespace kura::cpu
