# Kura — Predicted System Requirements

_Status: **predicted** — these are estimates made during planning, not measured benchmarks. Real numbers will be confirmed once Kura is running and can be tested on actual hardware._

## 1. Predicted host requirements (the PC running Kura)

| Component | Minimum | Recommended |
|---|---|---|
| GPU | NVIDIA RTX 20-series (2018) / AMD RX 5000 / Intel Arc | RTX 30-series or newer |
| VRAM | 6 GB | 6 GB (8 GB+ once game support matures) |
| CPU | 6 cores | 8 cores / 16 threads |
| RAM | 16 GB | 16 GB+ |
| Storage | SSD, ~10 GB free for firmware | NVMe SSD |
| OS | **Windows 11 x64** | Windows 11 x64, fully updated |
| Graphics API | **Vulkan 1.3** (drivers up to date) | — |

### Why these numbers

- **The real GPU requirement is Vulkan 1.3**, not a specific card. Kura translates guest RDNA 2 shaders to SPIR-V (design doc §5.8), so any modern GPU from any vendor runs the same code path — NVIDIA RTX is simply the most common baseline.
- **Every desktop RTX GPU ever made has ≥6 GB VRAM** (the RTX 2060's 6 GB is the floor), so "any RTX after 2018" and "6 GB VRAM" are effectively the same line in the sand.
- **6 GB covers the early milestones** (shell UI, moderate resolutions). Games with large texture sets will push the recommendation to 8 GB+ later.
- **CPU/RAM mirror the guest**: the emulated machine is 8 cores/16 threads with 16 GB of memory, and Kura runs one host thread per guest thread (design doc §8).
- **SSD expected** because the guest's software assumes fast storage; an HDD would cause artificial hitching in emulated workloads.

## 2. Reference development machine

The primary dev/test rig for Kura:

| Component | Spec | Status |
|---|---|---|
| GPU | **NVIDIA GeForce RTX 2080 Ti — 11 GB** | ✅ known good — clears every minimum, ~2× the VRAM recommendation |
| CPU | TBD | — |
| RAM | TBD | — |
| Storage | TBD | — |

## 3. Guest hardware being emulated (PS5)

For reference — what Kura has to reproduce in software:

| Subsystem | PS5 spec |
|---|---|
| CPU | AMD Zen 2, 8 cores / 16 threads, ~2.28 GHz, x86-64 |
| GPU | AMD RDNA 2, 36 CUs, ~2.23 GHz, GFX10.3-family ISA |
| Memory | 16 GB GDDR6 (unified), ~448 GB/s |
| Storage | Custom NVMe SSD (825 GB class) |
| OS | FreeBSD-derived userland |

---

_See [DESIGN.md](DESIGN.md) for the full architecture — §5.8 covers the host-GPU acceleration design, §8 the threading model._
