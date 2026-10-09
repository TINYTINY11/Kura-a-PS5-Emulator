# Changelog

All notable changes to Kura will be documented in this file.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

**M3 stage 1.5 — first-startup experience (emulator UI, not emulated output)**
- Settings module — tiny dependency-free key=value store per user (`%APPDATA%\Kura\kura.cfg`); remembers firmware path, log level, wizard completion; malformed/unknown lines tolerated (`core/common/settings`)
- `kura_boot.exe` — the power-on window: frameless Win11-style translucent (DWM acrylic backdrop, rounded corners, dark mode) Win32+GDI GUI with splash fade-in, first-run setup wizard (firmware picker + log level), and a staged boot checklist driven by the real `kura_pup` pipeline — power on → firmware located (real file size) → SLB2 parsed → decryption `[HALT]` at the encryption wall, with marquee progress bar, verdict + next-steps note, and the raw pipeline log behind "Show details" (`frontend/boot`)
- Subsequent launches detect saved settings and boot straight to the pipeline (wizard once, then remembered)
- Fixed on the way: child controls parented before window-handle assignment (invisible UI), black-on-black labels (dark-theme control colors), clipped verdict text
- Tests: `unit.settings` round-trip + malformed-input coverage — 8/8 suites green

**M2 stage 2 — interpreter depth + SELF/ELF groundwork**
- Multiply/divide family — one-operand MUL, IMUL, DIV, IDIV at 8/16/32/64-bit widths, full 128-bit support for 64-bit forms (hand-rolled 64×64→128 multiply and restoring 128÷64 division, since MSVC has no `__int128`); new `StopReason::DivideError` stops cleanly on /0 and quotient overflow (`core/cpu`)
- Shifts and rotates — SHL/SHR/SAR/ROL/ROR in all forms (imm, by-1, CL), architectural count masking, correct carry/overflow for count==1, direction flag now tracked (CLD/STD) (`core/cpu`)
- ADC/SBB now carry-in correct at every width (was a stub), reachable via the r/m, r · r, r/m · r/m, imm · accumulator forms
- Conditional moves and sets — CMOVcc (reads its memory operand even when the condition is false, matching real x86 fault semantics) and SETcc (`0F 40-4F` / `0F 90-9F`)
- Missing high-frequency compiler output: accumulator-immediate ALU forms (`05`/`3D`/… — `cmp eax,imm32` is everywhere), 8-bit immediate group `80`, IMUL-with-immediate (`69`/`6B`), XCHG, NOT/NEG, CLC/STC/CMC
- REP string ops — MOVS/STOS/LODS (`rep movsq`/`rep stosq` = memset/memcpy in every binary), forward and backward via DF (`core/cpu`)
- ELF: SCE `e_type` acceptance (`ET_SCE_EXEC 0xFE00` range used inside SELF containers) — community RE confirms these headers are plaintext inside decrypted components (`core/loader`)
- ELF: `find_embedded_elf` scanner — locates a valid ELF64 at an arbitrary offset inside a blob (fake/foreign magic skipped), for finding inner ELFs inside decrypted firmware components (`core/loader`)
- Tests: 14 new interpreter cases (carry chains, signed division remainders, high-word multiply, condition codes, rep strings, divide-error paths) + 2 new ELF cases — `unit.cpu` now at 25 checks-heavy programs, 7/7 suites green

**M1 firmware pipeline (stage 1 → 2 groundwork)**
- Modern PUP entry-table parser — community-documented 48-byte component records (id/offset/sizes/flags), plausibility-checked; readable on decrypted `.PUP.dec` files, reports "not readable" on encrypted containers by design (`kura_pup`)
- 14 known component IDs mapped to names (eap_kernel, kernel, bios, gpu_ucode, …)
- RE notes massively expanded: full community header layout cross-checked against our empirical table (0x0C = entry count, 0x10 = header size — retroactively explaining stage-1 "unknown" fields), two-layer encryption model (outer SLB2 + inner per-binary SELF AES), inner SELF segment layouts, and the 2025-12 BootROM key-leak situation with Kura's no-keys-in-repo policy (`docs/RE-pup.md`)

**Project**
- Design documentation — full architecture, subsystem designs, roadmap M0–M7, risk register, legal constraints (`docs/DESIGN.md`)
- Predicted system requirements — host specs, reference dev rig, guest PS5 specs (`docs/SPEC.md`)
- First boot experience — RPCS3-style `PS5UPDATE.PUP` firmware install flow, documented with translucent Windows 11 UI guidelines
- GitHub repository with `.gitignore` that blocks firmware (`*.PUP`), keys, and dumps from ever being committed

**M2 loader + interpreter (stage 1)**
- Guest memory manager — sparse x86-64 address space with bounds-checked, overflow-safe read/write, region overlap rejection (`core/memory`)
- ELF64 loader — pure, bounds-checked parser (ET_EXEC/ET_DYN, EM_X86_64) + PT_LOAD segment mapping into guest memory (`core/loader`)
- x86-64 interpreter — decode-and-execute loop over a practical instruction subset (MOV/LEA, ALU groups, Jcc, CALL/RET, PUSH/POP, TEST, MOVZX/MOVSX, SYSCALL) with flag-correct arithmetic and RIP-relative addressing (`core/cpu`)
- Syscall/exit hooks — guest `exit` (FreeBSD 1 / Linux 60) captured by the host; unhandled syscalls stop the run with context instead of guessing
- Tests: `unit.memory`, `unit.elf`, `unit.cpu` (hand-assembled guest programs), `unit.boot` — full path test: synthetic ELF → parse → map → execute → guest-computed exit code verified end-to-end
- Fault behavior: invalid opcode, unmapped fetch/load/store, and step limits all stop cleanly with diagnostics (never crash the host — §11)

**M1 firmware pipeline (stage 1)**
- First boot sequence: `kura --firmware <PUP>` runs stage-by-stage (locate → parse → decrypt attempt) with a live boot log; halts honestly at the encryption wall with next-step guidance
- `kura_pup` — standalone SLB2/PUP structure inspector (header parse, size validation, SHA-256-shaped digest dump, Shannon entropy analysis, full-file nested-magic block scan)
- RE notes documenting the empirical SLB2 v3 header layout and open questions (`docs/RE-pup.md`)

**M0 foundations (code)**
- CMake build system (C++20) with Visual Studio 2026 solution generator (`generate-sln.bat`)
- Logging/trace system — leveled (trace→error) and channel-based, with console (colored), file, and custom sinks (`core/common/log`)
- PSN network filter — deny-by-default block list for Sony/PSN hosts; neither Kura nor emulated software contacts PSN (`core/io/net_filter`)
- CLI entry point `kura` — `--log-level`, `--log-file`, `--firmware` (M1 stub), `--check-host`
- Double-click support: running `kura.exe` from Explorer keeps the window open ("Press Enter to exit"); terminal runs exit normally
- Unit tests via CTest (`unit.log`, `unit.net_filter`)
- GitHub Actions CI — builds and tests on Windows and Ubuntu
- Host safety guarantees documented (design doc §11): user mode only, process-local state, interpreter-safe CPU, bounded resources

[Unreleased]: https://github.com/TINYTINY11/Kura-a-PS5-Emulator/commits/main
