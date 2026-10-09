#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "memory/guest_memory.hpp"

namespace kura::kernel {

// --- open(2) flag values (FreeBSD; PS5 inherits them) ------------------------
inline constexpr std::uint32_t kOAccmode = 0x0003;
inline constexpr std::uint32_t kORead = 0x0000;
inline constexpr std::uint32_t kOWrite = 0x0001;
inline constexpr std::uint32_t kORdwr = 0x0002;
inline constexpr std::uint32_t kONonblock = 0x0004;
inline constexpr std::uint32_t kOAppend = 0x0008;
inline constexpr std::uint32_t kOCreat = 0x0200;
inline constexpr std::uint32_t kOTrunc = 0x0400;
inline constexpr std::uint32_t kOExcl = 0x0800;

// lseek whence
inline constexpr int kSeekSet = 0;
inline constexpr int kSeekCur = 1;
inline constexpr int kSeekEnd = 2;

// M3 stage 1 virtual file system.
//
// Scope: an in-memory namespace with POSIX-ish semantics. This is enough to
// host early-boot guest programs and to shape the fd/syscall plumbing; the
// bridge to real host files (sandboxed, per design doc §11) arrives in a
// later M3 stage. fd 0/1/2 exist from construction: stdin reads as EOF,
// stdout/stderr capture into a host-side buffer (and the Kura log).
//
// Error convention: methods return bytes moved on success, or -errno
// (FreeBSD style) on failure — the Kernel writes that straight into RAX.
class Vfs {
public:
    Vfs(); // fd 0/1/2 exist

    // >= 0: new fd; < 0: -errno
    int open(const std::string& path, std::uint32_t oflags);
    int close(int fd);

    std::int64_t read(int fd, GuestMemory& mem, std::uint64_t buf, std::uint64_t len);
    std::int64_t write(int fd, GuestMemory& mem, std::uint64_t buf, std::uint64_t len);
    std::int64_t lseek(int fd, std::int64_t offset, int whence);

    bool exists(const std::string& path) const;
    // fd validity probe (ioctl distinguishes EBADF from ENOTTY).
    bool has_fd(int fd) const { return fds_.find(fd) != fds_.end(); }
    // Host-side inspection (tests / debugger): full contents of a file.
    std::optional<std::vector<std::byte>> file_bytes(const std::string& path) const;
    // Bytes written to fd 1/2 since construction.
    const std::string& tty_text() const { return tty_text_; }

private:
    // An open descriptor. Contents live in files_ (the source of truth,
    // keyed by path) — the fd only tracks position and flags, so writes
    // through one fd are visible through the named store and to reopens.
    struct File {
        std::string path;
        std::uint64_t pos = 0;
        std::uint32_t flags = 0;
        bool tty = false;
    };

    File* get(int fd);
    std::map<std::string, std::vector<std::byte>> files_; // named store
    std::map<int, File> fds_;                             // open descriptors
    std::string tty_text_;
    int next_fd_ = 3;
};

} // namespace kura::kernel
