# Kura syscall coverage

Living coverage table for the kernel HLE (M3). Every number below is the
**predicted** FreeBSD amd64 value from public `sys/syscalls.tbl` sources —
each one gets a verification pass against real PS5 binaries when the
decryption wall falls (`docs/RE-pup.md`). Errors are FreeBSD errno
numbering (BSD-specific: `EAGAIN` = 35, `ENOSYS` = 78, `ENOTTY` = 25).

**Legend:** ✅ implemented and unit-tested · 🟡 placeholder semantics
(works, shape predicted) · ⭕ known number, returns `-ENOSYS` (real
binaries route around) · ⏳ planned

> Exit routing note: FreeBSD exit is syscall **1 only**. Linux's exit (60)
> coincides with FreeBSD `nanosleep`; treating it as an exit would kill a
> sleeping guest (fixed in M3 stage 2).

## Implemented

| # | name | status | notes |
|------:|---|---|---|
| 1 | `exit` | ✅ | interpreter exit hook → process marked exited |
| 3 | `read` | ✅ | in-memory store + host bridge (copy-on-open) |
| 4 | `write` | ✅ | fd 1/2 capture into `guest.out` log channel |
| 5 | `open` | ✅ | FreeBSD flags; `EISDIR`/`EFBIG`/`EEXIST` edges |
| 6 | `close` | ✅ | std fds protected |
| 20 | `getpid` | ✅ | process table |
| 24 | `getuid` | 🟡 | returns 0 (root placeholder — real uid map TBD) |
| 25 | `geteuid` | 🟡 | same |
| 26 | `getgid` | 🟡 | same |
| 27 | `getegid` | 🟡 | same |
| 54 | `ioctl` | 🟡 | `EBADF` / non-tty `ENOTTY`; termios shapes pending real binaries |
| 60 | `nanosleep` | ✅ | real sleep, clamped to 250 ms so a bad guest timer can't hang the window |
| 73 | `munmap` | ✅ | alignment + existence validated |
| 74 | `mprotect` | 🟡 | success-stub — no paging yet, revisit with MMU |
| 114 | `getppid` | ✅ | process table |
| 116 | `gettimeofday` | ✅ | host wall clock, `tz` ignored (FreeBSD behaviour) |
| 188 | `stat` | ✅ | predicted 160-byte `struct stat`, offsets locked by test |
| 189 | `fstat` | ✅ | `/dev/*` fds = char devices |
| 190 | `lstat` | ✅ | `== stat` — no symlinks in the stage-4 Vfs |
| 202 | `sysctl` | ✅ | read-only tree: `kern.osrelease`, `kern.argmax`, `hw.pagesize`, `hw.ncpu`; full `oldlenp` probe/short-buffer semantics |
| 232 | `clock_gettime` | ✅ | `CLOCK_REALTIME` + `CLOCK_MONOTONIC` (FreeBSD id 4) |
| 477 | `mmap` | ✅ | anonymous, `MAP_FIXED`, and **file-backed** (fd bytes via Vfs; tail past EOF zero-filled); `MAP_SHARED` file maps → `-ENOSYS` pending writeback |
| 478 | `lseek` | ✅ | `SEEK_SET/CUR/END` |

Also tolerated: any unknown number → `-ENOSYS` (logged at debug), which
real FreeBSD binaries treat as "feature absent".

## Next up (per DESIGN.md M3 exit criteria)

| area | status | notes |
|---|---|---|
| threads (`thr_new` family, FreeBSD 430s) | ⏳ | needs a scheduler + per-thread interpreter state; the M3 exit gate ("multi-threaded guest tests run") |
| futex / `__umtx_op` (~454, predicted) | ⏳ | pairs with threads |
| `wait4` (7, high confidence) | ⏳ | first real process-management syscall |
| dynlib (`sys_dynlib_*`, Sony-proprietary 600+ range) | ⏳ | numbers require PS5-specific research; stub family planned |
| signals (`sigaction`, `kill`, `sigreturn`) | ⏳ | delivery needs guest signal frames — after threads |
| syscall trace/coverage harness | ⏳ | DESIGN: "syscall trace is clean" is the M3 exit test |

## Verification policy

When decrypted firmware is available, each row gets checked against real
binary syscall traces. Expected failure mode of a wrong number: `-ENOSYS`
gracefully, never a crash — the guest routes around or halts at that call,
and the trace pinpoints the row to fix.
