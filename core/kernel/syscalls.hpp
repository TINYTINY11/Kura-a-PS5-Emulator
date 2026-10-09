#pragma once

#include <cstdint>

// FreeBSD amd64 syscall numbers and errno values.
//
// The PS5 kernel is a FreeBSD derivative: its *base* syscall table matches
// public FreeBSD amd64 numbering (sys/syscalls.tbl). Sony additions live in
// high ranges (600+) and are NOT included here yet — their numbers will be
// filled in from real-binary analysis once decrypted firmware is available.
//
// Status of these numbers: predicted from public FreeBSD sources. Every
// constant in this header gets a verification pass against real firmware
// binaries when the decryption wall falls (docs/RE-pup.md).
//
// amd64 syscall ABI: number in RAX; arguments in RDI, RSI, RDX, R10, R8, R9
// (RCX/R11 clobbered by the SYSCALL instruction itself). Return value in RAX;
// on error the kernel returns -errno directly in RAX.

namespace kura::kernel::sys {

// --- syscalls implemented through M3 stage 1 --------------------------------
inline constexpr std::uint64_t kExit = 1;         // exit(int code)
inline constexpr std::uint64_t kRead = 3;         // read(fd, buf, nbyte)
inline constexpr std::uint64_t kWrite = 4;        // write(fd, buf, nbyte)
inline constexpr std::uint64_t kOpen = 5;         // open(path, flags, mode)
inline constexpr std::uint64_t kClose = 6;        // close(fd)
inline constexpr std::uint64_t kGetpid = 20;      // getpid()
inline constexpr std::uint64_t kMunmap = 73;      // munmap(addr, len)
inline constexpr std::uint64_t kGettimeofday = 116; // gettimeofday(tv, tz)
inline constexpr std::uint64_t kMmap = 477;       // mmap(...) — see kernel.cpp
inline constexpr std::uint64_t kLseek = 478;      // lseek(fd, pad, off, whence)

// --- added in M3 stage 2 ----------------------------------------------------
inline constexpr std::uint64_t kIoctl = 54;      // ioctl(fd, req, argp)
inline constexpr std::uint64_t kNanosleep = 60;  // nanosleep(req, rem)
// NOTE: Linux uses 60 for its exit syscall — a PS5 (FreeBSD) binary never
// does. The interpreter must NOT treat 60 as an exit or a sleeping guest
// would be killed (fixed in M3 stage 2).
inline constexpr std::uint64_t kGetuid = 24;
inline constexpr std::uint64_t kGeteuid = 25;
inline constexpr std::uint64_t kGetgid = 26;
inline constexpr std::uint64_t kGetegid = 27;
inline constexpr std::uint64_t kGetppid = 114;   // getppid()
inline constexpr std::uint64_t kClockGettime = 232; // clock_gettime(id, ts)

// --- added in M3 stage 3 ----------------------------------------------------
inline constexpr std::uint64_t kStat = 188;   // stat(path, buf)
inline constexpr std::uint64_t kFstat = 189;  // fstat(fd, buf)
inline constexpr std::uint64_t kLstat = 190;  // lstat(path, buf) — no symlinks

// --- added in M3 stage 6 (threads + futex) ---------------------------------
inline constexpr std::uint64_t kThrNew = 431;  // thr_new(param, param_size)
inline constexpr std::uint64_t kThrExit = 432; // thr_exit(status) — noreturn
inline constexpr std::uint64_t kUmtx = 454;    // __umtx_op(...) — futex family
inline constexpr std::uint64_t kThrSelf = 456; // thr_self() -> tid

// __umtx_op opcodes (FreeBSD sys/umtx.h)
inline constexpr std::uint64_t kUmtxWait = 0;
inline constexpr std::uint64_t kUmtxWake = 1;

// Commonly reached but not yet implemented — these return -kENOSYS, which
// real binaries treat as "feature absent" and route around.
inline constexpr std::uint64_t kMprotect = 74;
inline constexpr std::uint64_t kSysctl = 202;

const char* name(std::uint64_t nr);

// --- FreeBSD errno (subset; values are part of the kernel ABI) ---------------
inline constexpr int kEPERM = 1;
inline constexpr int kENOENT = 2;
inline constexpr int kEINTR = 4;
inline constexpr int kEIO = 5;
inline constexpr int kEBADF = 9;
inline constexpr int kECHILD = 10;
inline constexpr int kENOMEM = 12;
inline constexpr int kEACCES = 13;
inline constexpr int kEFAULT = 14;
inline constexpr int kEBUSY = 16;
inline constexpr int kEEXIST = 17;
inline constexpr int kENODEV = 19;
inline constexpr int kENOTDIR = 20;
inline constexpr int kEISDIR = 21;
inline constexpr int kEINVAL = 22;
inline constexpr int kENFILE = 23;
inline constexpr int kEMFILE = 24;
inline constexpr int kENOTTY = 25;
inline constexpr int kEFBIG = 27;
inline constexpr int kENOSPC = 28;
inline constexpr int kESPIPE = 29;
inline constexpr int kEROFS = 30;
inline constexpr int kEPIPE = 32;
inline constexpr int kERANGE = 34;
inline constexpr int kEAGAIN = 35; // BSD numbering: EAGAIN is 35, not 11
inline constexpr int kENAMETOOLONG = 63;
inline constexpr int kENOSYS = 78;
inline constexpr int kEOVERFLOW = 84;

} // namespace kura::kernel::sys
