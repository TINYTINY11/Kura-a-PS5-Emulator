# Changelog

All notable changes to Kura will be documented in this file.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

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
