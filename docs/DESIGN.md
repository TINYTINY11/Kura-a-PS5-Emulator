# Kura — PS5 Emulator Design Document

**Status:** v0.1, planning phase
**Goal:** A C++20 emulator that boots dumped PS5 system software to the real home screen.
**Non-goal (for now):** Full commercial-game compatibility (that comes after the shell boots).

---

## 1. Goals and non-goals

### Goals
1. **Boot real PS5 firmware** (dumped from the designer's own console) through the kernel/user boundary far enough to run the system shell (`shellui`) and display the home screen.
2. **Full host-GPU acceleration**: every guest draw and compute dispatch runs on real hardware via Vulkan — Kura ships no software rasterizer, and host GPU headroom is used for extra features (see §5.8).
3. Clean **layered architecture** so subsystems can be replaced (interpreter → JIT, HLE → LLE) without rewriting the core.
4. **Cross-platform host**: Windows primary, Linux secondary.
5. Everything **traceable**: instruction traces, syscall traces, GPU command dumps — RE-by-emulator is the methodology.

### Non-goals (explicitly deferred)
- PSN/network services (stubbed).
- Security-processor (PSP/SSP) fidelity — we bypass, not emulate, secure boot.
- Game compatibility until M6 (shell boot) is achieved.
- Shipping any Sony code, keys, or firmware in the repository.

---

## 2. Definition of done: "launch the home screen"

The milestone is reached when, from a legally dumped firmware on the host:

```
./kura --firmware /path/to/dumped/firmware \
       --system-software eboot.bin
```

…produces, in order:

1. Kernel HLE initializes; guest `libkernel` + system `eboot` mapped and control transfers to its entry point.
2. System modules (`libSceVideoOut`, `libScePad`, `libSceGnm`/GNM-family, `libSceAudioOut`, …) resolve.
3. GPU produces a first presentable frame via a Vulkan swapchain.
4. The shell's dashboard renders: row of game tiles, top status bar, background wave animation.
5. Input via a host gamepad navigates tiles (movement + focus change), proving the event loop, pad HLE, and frame pacing all work end-to-end.

Audio and network are allowed to be silent/broken at this milestone.

---

## 3. Target platform summary

Everything below must be verified against real dumps during RE; treat as working assumptions.

| Subsystem | Assumption | Emulation impact |
|---|---|---|
| CPU | AMD Zen 2, 8C/16T, x86-64, AVX2 (AVX-512 to verify) | Guest ISA == host ISA on x86 hosts → interpreter feasible, native/JIT execution possible later |
| GPU | AMD RDNA 2, 36 CU, GFX10.3-family ISA | Need PM4 command parsing + **RDNA ISA → SPIR-V** shader translation |
| RAM | 16 GB GDDR6 unified | One guest address space, ~47-bit VA; direct host mapping |
| Storage | Custom NVMe SSD | Filesystem-level emulation is sufficient; no block-device emulation needed |
| OS | FreeBSD-derived userland | Syscall HLE modeled on FreeBSD semantics |
| Executables | Signed ELF-like (SELF lineage), dynamic | Custom loader + dynlib resolution |
| Firmware updates | PUP containers, encrypted/signed, SPKG packages | Offline decryption pipeline (tool, not runtime) |
| Kernel modules | Signed `.skprx` / `.sprx`-style modules | Either HLE the syscalls they wrap, or interpret them as guest code — see §5.6 |

**Key strategic fact:** the guest CPU is x86-64, same as typical hosts. This makes CPU emulation the *easiest* hard part, not the hardest. The hardest parts are (a) the GPU shader/command path and (b) syscall/kernel surface area of the system software.

---

## 4. Overall architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Frontend: CLI, config, asset paths, debugger/GDB stub      │
├─────────────────────────────────────────────────────────────┤
│  Emulator Core (single guest world)                         │
│  ┌───────────┐ ┌──────────┐ ┌──────────┐ ┌───────────────┐  │
│  │ CPU Core  │ │ Memory   │ │ Kernel   │ │ GPU           │  │
│  │ (backend- │ │ Manager  │ │ HLE      │ │ (CP/PM4 →     │  │
│  │  abstract)│ │          │ │ (syscalls│ │  RDNA→SPIR-V  │  │
│  │           │ │          │ │  threads)│ │  → Vulkan)    │  │
│  └───────────┘ └──────────┘ └──────────┘ └───────────────┘  │
│  ┌───────────┐ ┌──────────┐ ┌──────────┐ ┌───────────────┐  │
│  │ Loader    │ │ Library  │ │ Audio    │ │ FS / IO / Net │  │
│  │ (SELF,    │ │ HLE      │ │ (AT9-ish │ │ (sandbox,     │  │
│  │  dynlib)  │ │ (shims)  │ │  → host) │ │  save data)   │  │
│  └───────────┘ └──────────┘ └──────────┘ └───────────────┘  │
├─────────────────────────────────────────────────────────────┤
│  Platform layer: OS abstraction (Win32/POSIX), Vulkan,      │
│  host audio (WASAPI/SDL), host input (RawInput/SDL)         │
└─────────────────────────────────────────────────────────────┘
```

**Design rules**

1. **No subsystem talks to the host OS directly** — all host access goes through the platform layer (portability + testability).
2. **CPU never blocks on emulated I/O**: syscall HLE runs on the calling guest thread; long operations are completed asynchronously and signaled back through the guest's synchronization primitives.
3. **Every subsystem exposes a trace channel** with severity levels, so boot debugging is a matter of turning logs on, not attaching a debugger first.
4. **HLE where the surface is well-known, LLE where it's opaque**: syscalls and libraries are HLE; GPU commands and shaders are LLE (translated). Kernel `.skprx` modules are *not* executed by default — their functionality is re-implemented at the syscall boundary (see §5.6 for the fallback).

---

## 5. Subsystem designs

### 5.1 Platform layer (`core/platform/`)
- Thin interfaces: threads, mutex/condvar, events, high-resolution clocks, dynamic library loading, memory reservation (`mmap`/`VirtualAlloc` equivalents), file mapping.
- Host video: Vulkan surface creation (Win32 `HWND` / Xlib or Wayland).
- Host input: SDL3 (simplest cross-platform path) with a raw-input fallback.
- Everything else in the emulator depends only on these interfaces.

### 5.2 Firmware pipeline (`tools/firmware/`) — build-time, not runtime
A **separate CLI tool**, never linked into the emulator:

1. **PUP parser** — split the update container into system images.
2. **Image extraction** — reconstruct the system partition layout.
3. **Decryption** — apply keys obtained from the user's own console research; keys live in a local, git-ignored `keys/` directory (never committed, never redistributed).
4. **Output layout** — a directory tree the emulator mounts as its root filesystem:

```
firmware-out/
  system/         # system modules, libkernel, defaults
  app/            # shell / system applications (incl. shell eboot)
  data/           # fonts, localization, assets
```

**Legal note:** the tool ships with *no* keys and *no* firmware. Users supply both from hardware they own (§13).

### 5.3 Loader (`core/loader/`)
Responsibilities:

1. **SELF/ELF parsing** — headers, program headers, identify encrypted/dynamic segments.
2. **Segment mapping** — reserve guest VA, apply relocations, set up TLS template + image.
3. **Dynamic linking** — resolve `DT_NEEDED` against the firmware tree; for each guest library decide: *interpret it* (default) or *HLE-shim it* (§5.6).
4. **Entry state setup** — stack construction, `auxv`-like data, initial thread registers, jump to entry.

Design decision: the loader is **backend-agnostic** — it produces a `LoadedImage` (mapped segments, symbol table, imports/exports) that both the CPU core and the library-HLE resolver consume.

### 5.4 CPU core (`core/cpu/`)

**Design: an execution-backend abstraction from day one**, even though the first backend is an interpreter. This keeps the native/JIT decision reversible (your "just planning" answer → we don't lock it in now).

```
CpuBackend (interface)
  ├─ InterpretingBackend   // M2: decode–execute loop, single-step
  ├─ BlockJitBackend       // future: translate basic blocks
  └─ NativeBackend         // future: map guest x86-64 directly,
                           //        trap syscalls (#syscall / int 0x80),
                           //        guard memory for sandboxing
```

Requirements common to all backends:

- **Accurate state**: GPRs, SSE/AVX XMM/YMM, MXCSR, RFLAGS, FS/GS base, x87 (minimal), 16 architectural threads' worth of save/restore.
- **Precise exceptions** for the instructions software relies on (`cpuid` feature bits, `xgetbv`, `rdtsc` monotonicity).
- **Stop conditions**: watchpoints on registers/memory for the debugger; instruction-count breakpoints.
- **AVX-512 question**: PS5's Zen 2 has no AVX-512, so the guest won't use it — the decoder may safely assert-if-seen at first.
- **Host safety**: interpreter is naturally safe; the native backend requires reserved guest ranges + guard pages + a dedicated executable page pool for any JIT output.

Recommendation (revisit at M3): **interpreter until the shell boots**, because boot bugs are almost always kernel/syscall/shader bugs, and an interpreter gives single-step ground truth. Add a block JIT only when frame time demands it.

### 5.5 Memory manager (`core/memory/`)

- Owns the guest virtual address space as a sparse map: `guest VA → {host backing, prot, label, tag}`.
- **Direct mapping**: allocate one large host reservation and expose sub-ranges to the guest (essential if the native backend is ever used; harmless for the interpreter).
- **MMIO registry**: named device ranges (`videoout`, `pm4 ring`, `nvme-ish storage doorbell`, timers) registered by subsystems; reads/writes dispatch to device handlers.
- **Page-level fault handling**: used for copy-on-write save data, RAM-below-16GB aliasing (if present), and debugger guard pages.
- API for other subsystems: `map/unmap/protect/read/write/copy`, plus a **span-based fast path** for the CPU core to avoid per-access hashing (chunked direct pointer with fallback).

### 5.6 Kernel HLE (`core/kernel/`)

This is where "boots real firmware" lives or dies.

**Approach: syscall-level HLE in the FreeBSD style.** The guest issues syscalls (`syscall` instruction with a number); we dispatch to a C++ handler that maintains emulated kernel state.

Core pieces:

1. **Process/thread model** — one emulated process; N guest threads mapped 1:1 onto host threads (pool-backed), each with its own CPU-core state, errno, and per-thread TLS in guest memory.
2. **Synchronization** — emulated futex/umtx (with a real host futex underneath for performance), semaphores, RW locks, event queues. Guest-visible timeouts are host-timer driven; spurious wakeups are allowed (so must be correct in guest code anyway).
3. **Virtual memory syscalls** — `mmap/munmap/mprotect/madvise` routed to §5.5.
4. **Dynlib syscalls** — load/query guest libraries; the surface the shell depends on heavily.
5. **I/O** — fd table backed by host files within the firmware sandbox; `open/read/write/ioctl/poll/kqueue`-family.
6. **Tunables/misc** — the long tail (system calls for clock, resource limits, `random`, `thr_*`, jail params). Track coverage with a **syscall coverage table** printed at exit.

**Kernel modules (`.skprx`)** — two modes:
- **Mode A (default, M3+):** do not execute them; implement the functionality they provide at the syscall boundary. Requires knowing *which* syscalls the shell expects — discovered by RE + syscall tracing on real hardware/research dumps.
- **Mode B (escape hatch):** if a module's code is plain x86-64 with resolvable imports, load and interpret it as a guest library. Valuable when RE hasn't caught up. Make this a per-module switch in a manifest file.

**Discovery workflow:** maintain `docs/syscalls.md` — every number seen, its arg shape, whether handled/stubbed/crashing, plus the guest code site that called it. The emulator's own crash reports append to this.

### 5.7 Library HLE (`core/libraries/`)

Guest `libSce*.sprx` libraries call syscalls; we may substitute native shims for the ones with pure-translation semantics:

- **Always shimming (I/O-shaped):** `libSceVideoOut`, `libScePad`, `libSceAudioOut`, `libSceHttp*` (stub), `libSceSaveData` (host-directory-backed).
- **Leave as guest code where possible:** anything numeric/algorithmic — shimming risks behavior drift.
- Dispatch mechanism: a symbol table populated at load time; an unresolved import logs the library:symbol and, if configured, **fails loudly** with the caller's return address (fastest way to build the list of what must be shimmed next).

Ordering rule: **the shell forces the queue.** Whatever `libSce*` imports block shell boot get implemented next — priority comes from actual boot failures, not guesswork.

### 5.8 GPU (`core/gpu/`) — highest-risk subsystem

Pipeline (three stages, each independently testable):

```
guest ring buffer (guest RAM)
   │  read
   ▼
[1] Command Processor      parse PM4-family packets: draws, dispatches,
   │                        state writes (descriptor libs, RNGR, blend),
   │                        render-pass boundaries, EOP flush
   ▼
[2] ISA translation        RDNA 2 (GFX10.3) guest shaders → SPIR-V
   │                        (one-time cache, keyed by shader hash)
   ▼
[3] Vulkan backend         pipeline creation, descriptor set mapping,
                           buffer/image upload, barriers, present
```

**Host GPU utilization — Kura's core performance principle:**

- **Zero software rendering.** Stage [3] is a 1:1 mapping onto Vulkan, so guest rasterization, compute, and texture work executes on the host GPU at full hardware speed. Performance scales directly with the user's GPU — a faster host GPU renders the guest's frames faster, not just at higher resolution.
- **Any Vulkan-capable GPU works.** Guest RDNA 2 shaders are translated to SPIR-V (portable intermediate), so NVIDIA, AMD, and Intel hosts all run the same code path; AMD RDNA hosts are simply the closest relative to the guest ISA.
- **Headroom is spent, not wasted.** Because the host GPU typically outclasses the PS5's 36-CU part, spare capacity goes to:
  - **Resolution scaling** — render the guest's framebuffers at 2×/4× internal resolution before present.
  - **Post-process options** — host-side filtering (FSR-style upscaling, sharpening, frame pacing) as a present-pass step, entirely opt-in.
  - **Asynchronous overlap** — guest compute and graphics submissions map to Vulkan async queues where the guest's ordering allows it, overlapping work the same way real hardware would.
- **Shader result caching.** Translated SPIR-V → host-driver compiled pipelines are cached on disk keyed by guest-shader hash, so after the first boot the host GPU's driver does the compilation once and Kura starts near full speed.
- **Deterministic-by-default mode** for debugging: an option forces synchronized, single-queue submission so GPU race conditions become reproducible.

Key decisions:

- **Vulkan only** (no D3D12 initially). One backend, maximum portability, mature SPIR-V tooling — and the direct path to running everything on the host GPU with no intermediary layers.
- **Shader translation is the long pole.** Options:
  - *Self-built decoder* (full control, biggest effort).
  - *Leverage existing open-source RDNA→SPIR-V/GCN tooling* where licensing permits, then close gaps.
  - Track what open-source PS4-era emulators did for GCN and port the approach to RDNA 2 (extra wave ops, newer encoding).
  - Cache aggressively: system software has few unique shaders; booting the shell may require translating only tens-to-hundreds.
- **State emulation maps 1:1 to Vulkan**: pixel/vertex/compute only at first; tessellation/geometry stages deferred (the shell likely doesn't need them).
- **Sync model:** treat EOP/flush packets as Vulkan barriers; do *not* try to emulate clock cycles — correctness first, then measure.
- **Swapchain & videoout:** `libSceVideoOut` flips present the newest complete framebuffer; implement double/triple buffering semantics against the actual vsync cadence of the host.

### 5.9 Audio (`core/audio/`)
- Guest writes into an emulated audio ring / issues `audioOut` outputs → host callback (WASAPI/SDL).
- Codec support (AT9-style) only if the shell requires it for startup sounds; otherwise stub output as silent-but-advancing (never block the guest).

### 5.10 Filesystem, input, network (`core/io/`)
- **FS:** host-directory sandbox mirroring the firmware tree + writable save locations; path translation layer handles the guest's absolute layout (`/app0`, `/system`, …).
- **Input:** host gamepad → emulated DualSense/DualShock4 state structure (connection + report), delivered through the pad HLE event queue.
- **Network:** stub `sceNet`/`sceHttp` with correct error codes; no packets until post-shell.

### 5.11 Debugger & tooling (`core/debug/`) — built in, not bolted on
- **Trace channels**: `cpu.sys`, `cpu.insn` (windowed), `gpu.cmd`, `gpu.shader`, `kernel.sys`, `kernel.sync`, `loader`, `audio`.
- **GDB remote stub** (or LLDB) for guest breakpoints: standard tooling immediately usable.
- **Frame capture**: dump a GPU frame's command list + all referenced resources/shaders to disk for offline replay (indispensable for shader bugs).
- **Crash forensics**: on guest fault, print last N syscalls, last M instructions, current draw, symbolized guest backtrace.
- **RE scratchpad**: `docs/` runs in parallel with code — every discovered structure, syscall, and packet gets a note; the emulator is the forcing function for that knowledge.

---

## 6. Boot sequence (runtime flow)

```
Stage 0  Host init: config, platform, Vulkan, audio, input
Stage 1  Firmware tree validation (run tools/firmware first if missing)
Stage 2  Create guest world: memory map, kernel HLE state, CPU threads
Stage 3  Load system eboot (loader §5.3) → resolve imports (§5.6/5.7)
Stage 4  Start main guest thread at entry point
Stage 5  Kernel/lib services come up as guest code calls them
         (videoout → pad → audio → GPU ring → shell main loop)
Stage 6  First present: Vulkan frame on screen
Stage 7  Home screen interactive (M6 exit criteria)
```

Each stage has a **named checkpoint** with an expected log line, so a failure tells you immediately *which* subsystem to work on.

---

## 7. Codebase layout

```
kura/
  CMakeLists.txt
  docs/                  # DESIGN.md, syscall notes, RE findings, boot logs
  tools/
    firmware/            # PUP/SPKG extractor + decryptor (standalone CLI)
    selfdump/            # helper utilities
  core/
    platform/            # OS abstraction
    common/              # logging, tracing, hashing, containers, result types
    loader/              # SELF/ELF, dynlib, image model
    cpu/                 # backend interface, interpreter, (later) JIT/native
    memory/              # guest VA, MMIO registry
    kernel/              # syscalls, threads, sync, fd table, process state
    libraries/           # libSce HLE shims, grouped by domain
    gpu/                 # CP/PM4 parser, ISA translator, Vulkan backend
    audio/
    io/                  # fs, pad, save data, net stubs
    debug/               # traces, GDB stub, frame capture
  frontend/
    cli/                 # main(), argument/config handling
    ui/                  # optional: window/debug overlay
  tests/
    unit/                # per-subsystem tests
    integration/         # guest test binaries with expected output
  keys/                  # git-ignored, user-supplied
  firmware/              # git-ignored, user-supplied dumps
```

Build: **CMake + vcpkg/Conan** (Vulkan SDK, SDL3, zstd, Catch2/GoogleTest). CI builds Windows + Linux and runs unit tests; integration tests skip gracefully when no firmware is present (CI must never contain Sony code).

---

## 8. Concurrency model

- **1 guest thread : 1 host thread** (pinned pool). Simple, correct, and matches how real multi-threaded shells behave.
- **GPU runs on its own host thread** with a command queue fed by whichever guest thread submits (mirrors real CP behavior without blocking guests).
- **Shared state rules:** subsystem-internal mutexes; cross-subsystem interaction only via queues/messages; the CPU core's fast path touches no global locks.
- **Ordering hazards to design for up front:** futex wake ordering, timer cancellation races, GPU-vs-CPU memory visibility (guest may poll a completion value — must be a properly synchronized host variable).
- Avoid time-based logic in the core — everything derived from an **emulated clock** that the frontend can scale/pause (great for debugging: single-frame stepping).

---

## 9. Testing strategy

1. **Unit tests with zero firmware required**: PUP parser on synthetic files, ELF loader on public test binaries, syscall handlers against a mocked process state, memory map edge cases, PM4 parser on hand-written packets.
2. **Guest test binaries**: tiny x86-64 ELF/SELF-ish programs built in-repo (`tests/guest/`) — "does `mmap` + `write` + `futex` work end-to-end?"
3. **Golden traces**: recorded trace of a boot attempt checked into `tests/golden/` (structure only, no Sony bytes), diffed to catch regressions.
4. **Shader conformance**: render a known scene, compare screenshots with tolerance.
5. **Bug workflow**: every shell-boot blocker becomes a minimized test when possible — the test suite *is* the roadmap's memory.

---

## 10. Roadmap

| Milestone | Deliverable | Exit criteria |
|---|---|---|
| **M0** Foundations | Repo, CI, logging/tracing, platform layer, Vulkan+SDL window | Green CI on Win/Linux; trace channels working |
| **M1** Firmware tool | PUP/SPKG extraction + decryption pipeline | Produces a valid firmware tree from a user dump |
| **M2** Loader + interpreter | SELF loader, memory manager, interpreter runs test ELFs | Guest test binaries execute and pass in CI |
| **M3** Kernel HLE core | Threads, futex, mmap, fd I/O, dynlib syscalls, syscall coverage table | Multi-threaded guest tests run; syscall trace is clean |
| **M4** GPU first pixels | PM4 parser + RDNA→SPIR-V + Vulkan draws | Homebrew GNM-family triangle → textured scene renders |
| **M5** Service HLE | VideoOut, Pad, AudioOut, SaveData shims; first *real* system binary attempted | System software reaches a known early stage; syscall gaps enumerated |
| **M6** Shell boot | Resolve remaining gaps until `shellui` main loop runs | **Home screen renders and responds to pad input** ← primary goal |
| **M7+ | Audio polish, frame pacing, app launch, then games | — |

Sequencing rationale: M4 (GPU) is started in parallel with M3 by whoever isn't blocked, because it's the long pole; shader work benefits from a running frame loop early.

---

## 11. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| RDNA 2 shader translation too large | Blocks M6 entirely | Start with degenerate/tiny shaders, reuse open-source GCN/RDNA work, run the shell in degraded pipeline modes first |
| Unknown syscalls/structures halt boot | Slow progress | Crash-forensics + coverage table (§5.6) make each blocker a small, documented task |
| Timing-sensitive shell code (waits on GPU/audio ticks) | Subtle hangs | Emulated clock + pause/step debugging; make timers deliberately generous early on |
| Encrypted/signed content assumptions wrong | Loader stalls | Keep Mode B (interpret guest code as-is) viable; adjust as RE reveals formats |
| Solo scope creep | Never finishes | Shell-first priority rule; **no game work until M6** |
| Native-execution backend security (if adopted) | Host compromise risk | Ship interpreter-only by default; native backend opt-in, sandboxed |

---

## 12. Legal / ethical constraints (read before M1)

- **Emulation itself is lawful** in key jurisdictions (precedents: *Sega v. Accolade*, *Sony v. Connectix*), and clean-room reimplementation from observation is the accepted model.
- **Do not redistribute** firmware, keys, games, or any Sony-copyrighted bytes — not in the repo, not in tests, not in CI, not in screenshots of our own tests.
- Firmware/keys come from **consoles and content the user owns**; the extraction tool documents that requirement and bundles no secrets.
- Document your sources of RE knowledge in `docs/` — independent derivation matters.
- Keep firmware-dependent tests **local-only**; CI runs the firmware-free suites.

---

## 13. Reference / prior art to study first

- **shadPS4** — closest analogue: PS4 (also FreeBSD + x86-64 + AMD GPU) HLE kernel, syscall coverage, GNM→Vulkan. Directly informs §5.6/§5.8.
- **fpPS4** — HLE-focused PS4 emulator; library HLE patterns.
- **Vita3K / Ryujinx / Yuzu-era writeups** — HLE module dispatch and firmware layout approaches.
- **PS4/PS5 homebrew & research scene** (payload SDKs, syscall documentation projects) — syscall numbers/shapes, SELF formats, debug techniques.
- **AMD public docs**: RDNA 2 ISA reference, GFX10.3 register/PM4 documentation, Vulkan spec.
- **FreeBSD source** (syscall semantics) — the guest kernel's ancestry.

---

## 14. Open decisions (parked for later)

1. **CPU backend**: default plan is interpreter → optional block JIT; native execution deferred and opt-in. Decide at M3 exit with interpreter profile data in hand.
2. **Module strategy mix**: how much of `.skprx` becomes HLE vs Mode B guest execution — revisit at M5 with real boot traces.
3. **Host input stack**: SDL3 vs native per-platform — decide in M0 (lean SDL3 now).
4. **License**: matters if aiming for community contribution; recommend permissive (MIT/Apache-2.0) or GPLv3 depending on linking ambitions — decide before the first public push.
5. **Window/debug UI**: ImGui overlay vs external tools — decide in M0 (lean ImGui overlay for stats).

---

*Next step after this document: M0 scaffolding (repo layout + CI + trace system), whenever you're ready.*
