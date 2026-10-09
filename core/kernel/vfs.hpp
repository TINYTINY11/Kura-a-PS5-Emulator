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

// POSIX file-type bits in mode_t (FreeBSD values)
inline constexpr std::uint32_t kS_IFCHR = 0x2000;
inline constexpr std::uint32_t kS_IFDIR = 0x4000;
inline constexpr std::uint32_t kS_IFREG = 0x8000;

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

    // File metadata behind stat(2)/fstat(2) (M3 stage 3).
    struct StatInfo {
        std::uint64_t ino = 0;
        std::uint64_t size = 0;
        std::int64_t mtime_sec = 0;
        std::int32_t mtime_nsec = 0;
        std::uint32_t mode_perm = 0644;
        bool is_dir = false;
        bool is_char = false; // /dev/* nodes (stdin/stdout/stderr today)
    };
    // false = ENOENT (path) / EBADF (fd)
    bool stat_path(const std::string& path, StatInfo& out) const;
    bool stat_fd(int fd, StatInfo& out) const;

    // Host-side inspection (tests / debugger): full contents of a file.
    std::optional<std::vector<std::byte>> file_bytes(const std::string& path) const;
    // Bytes written to fd 1/2 since construction.
    const std::string& tty_text() const { return tty_text_; }

private:
    // One stored file. Contents live here (source of truth, keyed by
    // path) — an fd only tracks position and flags, so writes through one
    // fd are visible through the named store and to reopens.
    struct Node {
        std::vector<std::byte> data;
        std::uint64_t ino = 0;
        std::int64_t mtime_sec = 0;
        std::int32_t mtime_nsec = 0;
    };

    struct File {
        std::string path;
        std::uint64_t pos = 0;
        std::uint32_t flags = 0;
        bool tty = false;
    };

    File* get(int fd);
    StatInfo info_of(const Node& n) const;
    std::map<std::string, Node> files_; // named store
    std::map<int, File> fds_;           // open descriptors
    std::string tty_text_;
    int next_fd_ = 3;
    std::uint64_t next_ino_ = 1;
};

} // namespace kura::kernel
