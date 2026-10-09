# RE notes — PS5UPDATE.PUP / SLB2 container

_Observations from local analysis of a retail PS5 update file (user-supplied, never committed). Sources: empirical byte analysis on 2026-10-09; community research published June 2026 (Se7enSins "PS5 PUP Format Reverse Engineered" full breakdown + psdevwiki update, tools `pup_extract.py` / `analyse_extracted.py`); reporting on the December 2025 BootROM key disclosure (KitGuru, Tech Insider, Insider Gaming via Reddit r/PS5)._

## Findings (verified against the file itself)

| Offset | Type | Value observed | Meaning / confidence |
|---|---|---|---|
| 0x00 | char[4] | `SLB2` | magic — **confirmed** |
| 0x04 | u32 LE | 3 | version — high confidence |
| 0x08 | u32 LE | 0x10000 | block size (64 KiB) — high confidence (used for block scan) |
| 0x0C | u32 LE | 1 | **entry count** (matches community layout — confirmed 2026-10) |
| 0x10 | u64 LE | 0x2597C8 (2,463,688) | **header size** — matches community layout field at same offset |
| 0x18 | u64 LE | 0 | data size (community layout) — 0 here, consistent with encrypted container |
| 0x20 | u32 LE | 2 | padding region per community layout |
| 0x24 | u32 LE | 1,261,406,781 | near-file-size (actual file: 1,261,408,256 — **1,475 bytes larger**). Open question: payload-only size? trailer excluded? |
| 0x30 | char[16] | `PS5UPDATE1.PUP` | image name (legacy field) — **confirmed** |
| 0x200 | 32 B | `4c294437…cd03` | digest (SHA-256-shaped) over what exactly — unknown |
| 0x3E0 | payload | entropy ≈ 7.997/8.0 | **encrypted** — near-max Shannon entropy everywhere sampled |

## Community layout (published June 2026 — cross-check against our empirical table)

A full RE of the 5.50 update PUP established the decrypted-container layout. Our
field offsets agree where they overlap (entry count @0x0C, header size @0x10),
which retroactively explains our "unknown" fields.

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | magic `SLB2` |
| 0x04 | 4 | version |
| 0x08 | 4 | mode |
| 0x0C | 4 | entry count |
| 0x10 | 8 | header size |
| 0x18 | 8 | data size |
| 0x20 | 16 | padding |
| 0x30 | count × 0x30 | **entry table** |

Each 48-byte entry: `id:u64, compressed_size:u64, uncompressed_size:u64,
offset:u64, flags:u32 (bit0=compressed, bit1=signed, bit2=encrypted), pad[12]`.

Known entry IDs (5.50, community-documented; now in `kura_pup`):

| ID | Name | Notes |
|---|---|---|
| 0x100 | eap_kernel | Aeolia A53 co-processor OS (~330–466 MB × 5 copies) |
| 0x200 | emc_ipl | Aeolia embedded controller; contains x86-64 SELF |
| 0x300 | tee | trusted execution environment |
| 0x400 | bios | main system BIOS (multiple variants) |
| 0x600 | smf_ipl | system management firmware IPL |
| 0x1000 | kernel | main PS5 kernel |
| 0x1100 | bootloader0 | first-stage bootloader |
| 0x1300/0x1400 | psp_bl / psp_kernel | security processor stages |
| 0x2000 | update_package | small update blobs |
| 0x4000 | version_info | per-component version tags |
| 0x5000 | swu_manifest | software update manifest |
| 0x20000 | gpu_ucode | GPU microcode (11 identical copies in 5.50) |
| 0x30000 | sio_firmware | system I/O controller firmware |

## The encryption situation (two layers)

This is the crucial structural finding, and it matches our entropy results:

1. **Outer SLB2 container** — removed by "decrypting the PUP" → a `.PUP.dec`
   file with a readable entry table. Our retail file fails exactly here
   (entropy ≈ 7.997 from 0x3E0 onward, entry table unreadable).
2. **Inner SELF segments** — every extracted component is a PS5 SELF
   (`ET_SCE_EXEC`, 0xFE00) whose **headers are plaintext** (ELF structure,
   segment VAs, entry points all readable) but whose **segment data is
   per-binary AES-encrypted** with keys held in Aeolia secure storage.
   Segment entropy 7.95–7.997.

Practical consequence for Kura: even with a `.dec` file, code segments stay
encrypted — but structural metadata (load addresses, segment sizes, entry
points) is already useful for M2/M3 planning, and `kura_pup` can now read it.

Inner SELF details recovered by the community (emc_ipl/smf_ipl share an
identical embedded x86-64 SELF: entry VA 0x400080, 6 PT_LOAD-style segments,
execute-only segment 0 (no-read hardening), BSS tail on segment 2, zero
section headers). An "encrypted segment info table" precedes the ELF header
with entries whose offsets end in `0x0006` and heavy padding (anti-analysis).

## Keys situation (2025-12 → 2026-01, from news reporting)

- **2021**: fail0verflow publicly stated they obtained all symmetric root keys
  via software; they never published them.
- **Late Dec 2025 / early Jan 2026**: PS5 **BootROM keys leaked** online
  (hardware root-of-trust material, identical across all revisions to date).
  Per reporting: allows decrypting early-stage bootloader images; does **not**
  include the asymmetric keys; no complete public firmware-decryption chain
  has been published as of 2026-06. Inner SELF per-binary keys live in Aeolia
  secure storage — their presence in the leak is not established by any
  source found.
- Console-side tools (zecoxao's `ps5-pup-decrypt`, Oct 2022) produce `.dec`
  files **using a PS5 itself** (backup/restore flow). The user has no console,
  so this path is closed for us unless a `.dec` file is obtained elsewhere.

**Kura policy (design doc §12 + repo hygiene):** Kura never contains key
material, never downloads it, and does not fetch `.dec` files. If a user can
legitimately produce or obtain a decrypted PUP, `kura_pup` will parse and
extract it via the modern layout; the pipeline continues from there. The
encrypted-retail path remains blocked on keys we are not going to hardcode.

## Open questions (next RE session)

- The 1,475-byte size delta (trailer? alignment? footer with QR?)
- Digest coverage (header only vs whole file)
- Exact cipher/key-schedule for the outer container (no public source names
  the algorithm concretely — "AES" is the community assumption from entropy
  alone)
- Encrypted-segment-info-table format (entries at …0x0006 offsets)

## Tooling

- `kura_pup <file>` in `tools/firmware/` reproduces the empirical analysis
  **and** now parses the modern entry table when readable (decrypted files);
  on encrypted files it reports "entry table: not readable" — by design.
- Unit tests use **synthetic headers and tables only** — no real firmware
  bytes in the repo.
