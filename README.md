# Kura

**Kura** is a PlayStation 5 emulator written in C++20, with the goal of booting real dumped PS5 system software all the way to the interactive home screen.

## Status

Early planning / pre-alpha. See [docs/DESIGN.md](docs/DESIGN.md) for the full architecture, roadmap (M0–M7), and design decisions.

## Design highlights

- **Layered core** — CPU backend abstraction (interpreter first, JIT/native later), kernel HLE, library HLE, and an LLE GPU path that can evolve independently.
- **Host-GPU accelerated** — all guest rendering and compute runs in hardware via Vulkan; no software rasterizer. Resolution scaling and post-processing use the host GPU's headroom.
- **Firmware-driven** — boots a firmware tree extracted from a console you own; no keys or Sony code are ever included in this repository.

## System requirements (proposed — pre-alpha targets, not yet measured)

| Component | Minimum | Recommended |
|---|---|---|
| GPU | NVIDIA RTX 20-series (2018) / AMD RX 5000 / Intel Arc — anything with **Vulkan 1.3** | RTX 30-series or newer |
| VRAM | 6 GB | 6 GB (8 GB+ once game support matures) |
| CPU | 6 cores | 8 cores / 16 threads (guest is 8C/16T Zen 2) |
| RAM | 16 GB (guest has 16 GB GDDR6) | 16 GB+ |
| Storage | SSD, ~10 GB free for firmware | NVMe SSD (matches the guest's fast-storage assumptions) |
| OS | Windows 10/11 64-bit, or Linux with Vulkan 1.3 drivers | — |

_Virtualization note: these targets apply to the host-GPU accelerated design (§5.8 of the design doc) — all guest rendering runs in hardware, so any modern Vulkan GPU qualifies._

## Building

_Not yet — M0 (repo scaffolding, CI, tracing) is the first milestone._

## Legal

- Emulator code in this repository is original work.
- This project ships **no** firmware, encryption keys, or any Sony-copyrighted material.
- You supply the official `PS5UPDATE.PUP` (publicly distributed by Sony's update servers) and a `keys/` directory obtained from your own console or published research. See `docs/DESIGN.md` §12 before contributing.

## License

TBD — decide before the first public push.
