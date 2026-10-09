# RE notes — PS5UPDATE.PUP / SLB2 container

_Observations from local analysis of a retail PS5 update file (user-supplied, never committed). Sources: empirical byte analysis on 2026-10-09. Format reference cross-check: psdevwiki SLB2 page (Cloudflare-blocked at time of writing — empirical only)._

## Findings (verified against the file itself)

| Offset | Type | Value observed | Meaning / confidence |
|---|---|---|---|
| 0x00 | char[4] | `SLB2` | magic — **confirmed** |
| 0x04 | u32 LE | 3 | version — high confidence |
| 0x08 | u32 LE | 0x10000 | block size (64 KiB) — high confidence (used for block scan) |
| 0x0C | u32 LE | 1 | unknown — noted |
| 0x10 | u64 LE | 0x2597C8 (2,463,688) | likely metadata-region size — **hypothesis**, boundary sample shows payload continues encrypted past it |
| 0x18 | u64 LE | 0 | unknown |
| 0x20 | u32 LE | 2 | unknown |
| 0x24 | u32 LE | 1,261,406,781 | near-file-size (actual file: 1,261,408,256 — **1,475 bytes larger**). Open question: payload-only size? trailer excluded? |
| 0x30 | char[16] | `PS5UPDATE1.PUP` | image name — **confirmed** (printable, NUL-terminated) |
| 0x200 | 32 B | `4c294437…cd03` | digest (SHA-256-shaped) over what exactly — unknown |
| 0x3E0 | payload | entropy ≈ 7.997/8.0 | **encrypted** — near-max Shannon entropy everywhere sampled |

## Entropy results (64 KiB windows)

| Region | Bits/byte | Classification |
|---|---|---|
| Header window @0x0 | 7.977 | mixed (first 64 B are structured, rest of window is payload) |
| Payload start @0x3E0 | 7.997 | encrypted |
| Boundary @0x2597C8 | 7.997 | encrypted |
| 25% / 50% / tail | 7.997 | encrypted |

## Block scan

- 19,248 blocks of 64 KiB scanned (full file, one pass)
- Exactly **1** SLB2 magic found: offset 0x0 → no plaintext nested containers

## Conclusions

1. The file is a genuine retail PS5 update image, SLB2 v3 container.
2. **Everything after the 64-byte header is encrypted** — Stage 2 of the firmware
   pipeline (extract/decrypt) requires keys (design doc §5.2, §12 — published
   research path; user has no console).
3. Open questions for next RE session:
   - What the 1,475-byte size delta means (trailer? alignment? footer with QR?)
   - Field roles at 0x0C/0x18/0x20 — likely entry-table header, needs the
     decrypted metadata region (0x3E0–0x2597C8?) to resolve.
   - Digest coverage (header only vs whole file).

## Tooling

- `kura_pup <file>` in `tools/firmware/` reproduces all of the above.
- Unit tests use **synthetic headers only** — no real firmware bytes in the repo.
