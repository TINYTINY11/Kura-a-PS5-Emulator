# Kura

**Kura** is a PlayStation 5 emulator written in C++20, with the goal of booting real dumped PS5 system software all the way to the interactive home screen.

## Status

Early planning / pre-alpha. See [docs/DESIGN.md](docs/DESIGN.md) for the full architecture, roadmap (M0–M7), and design decisions.

## Design highlights

- **Layered core** — CPU backend abstraction (interpreter first, JIT/native later), kernel HLE, library HLE, and an LLE GPU path that can evolve independently.
- **Host-GPU accelerated** — all guest rendering and compute runs in hardware via Vulkan; no software rasterizer. Resolution scaling and post-processing use the host GPU's headroom.
- **Firmware-driven** — boots a firmware tree extracted from a console you own; no keys or Sony code are ever included in this repository.

## System requirements (proposed)

**GPU:** anything with Vulkan 1.3 — NVIDIA RTX 20-series (2018) / AMD RX 5000 / Intel Arc or newer · **VRAM:** 6 GB · **CPU:** 6+ cores · **RAM:** 16 GB · **Storage:** SSD

Full breakdown — including the reference dev rig and the PS5 specs being emulated — is in [docs/SPEC.md](docs/SPEC.md).

## Building

_Not yet — M0 (repo scaffolding, CI, tracing) is the first milestone._

## Legal

- Emulator code in this repository is original work.
- This project ships **no** firmware, encryption keys, or any Sony-copyrighted material.
- You supply the official `PS5UPDATE.PUP` (publicly distributed by Sony's update servers) and a `keys/` directory obtained from your own console or published research. See `docs/DESIGN.md` §12 before contributing.

## License

TBD — decide before the first public push.
