#include "kernel/vfs.hpp"

#include <algorithm>
#include <string_view>

#include "common/log.hpp"
#include "kernel/syscalls.hpp"

namespace kura::kernel {

Vfs::Vfs() {
    File in;
    in.path = "/dev/stdin";
    in.flags = kORead;
    fds_[0] = std::move(in);
    for (int fd : {1, 2}) {
        File tty;
        tty.path = (fd == 1) ? "/dev/stdout" : "/dev/stderr";
        tty.flags = kOWrite;
        tty.tty = true;
        fds_[fd] = std::move(tty);
    }
}

Vfs::File* Vfs::get(int fd) {
    auto it = fds_.find(fd);
    return it == fds_.end() ? nullptr : &it->second;
}

bool Vfs::exists(const std::string& path) const {
    return files_.find(path) != files_.end();
}

int Vfs::open(const std::string& path, std::uint32_t oflags) {
    if (path.empty()) return -sys::kENOENT;
    const bool creat = (oflags & kOCreat) != 0;
    const bool excl = (oflags & kOExcl) != 0;
    const bool acc_wr = (oflags & kOAccmode) != kORead;

    auto it = files_.find(path);
    if (it != files_.end()) {
        if (creat && excl) return -sys::kEEXIST;
        if (acc_wr && (oflags & kOTrunc)) it->second.clear();
    } else {
        if (!creat) return -sys::kENOENT;
        it = files_.emplace(path, std::vector<std::byte>{}).first;
    }

    File f;
    f.path = path;
    f.flags = oflags;
    f.pos = ((oflags & kOAppend) != 0) ? it->second.size() : 0;
    const int fd = next_fd_++;
    fds_[fd] = std::move(f);
    return fd;
}

int Vfs::close(int fd) {
    if (fd < 3 || get(fd) == nullptr) return -sys::kEBADF;
    fds_.erase(fd);
    return 0;
}

std::int64_t Vfs::read(int fd, GuestMemory& mem, std::uint64_t buf, std::uint64_t len) {
    File* f = get(fd);
    if (f == nullptr) return -sys::kEBADF;
    if (f->tty) return 0; // stdin-as-tty reads as EOF for now
    if (len == 0) return 0;

    auto it = files_.find(f->path);
    if (it == files_.end()) return 0; // empty backing store (e.g. /dev/stdin)
    auto& data = it->second;

    const std::uint64_t avail =
        (f->pos < data.size()) ? data.size() - f->pos : 0;
    const std::uint64_t n = std::min<std::uint64_t>(len, avail);
    if (n > 0) {
        if (!mem.write(buf, data.data() + f->pos, n)) return -sys::kEFAULT;
        f->pos += n;
    }
    return static_cast<std::int64_t>(n);
}

std::int64_t Vfs::write(int fd, GuestMemory& mem, std::uint64_t buf, std::uint64_t len) {
    File* f = get(fd);
    if (f == nullptr) return -sys::kEBADF;
    if ((f->flags & kOAccmode) == kORead) return -sys::kEBADF;
    if (len == 0) return 0;

    std::vector<std::byte> tmp(static_cast<std::size_t>(len));
    if (!mem.read(buf, tmp.data(), len)) return -sys::kEFAULT;

    if (f->tty) {
        const std::string_view text(reinterpret_cast<const char*>(tmp.data()),
                                    static_cast<std::size_t>(len));
        tty_text_.append(text);
        log::info("guest.out", text);
        return static_cast<std::int64_t>(len);
    }

    auto it = files_.find(f->path);
    if (it == files_.end()) return -sys::kEBADF; // no backing store
    auto& data = it->second;
    if (f->pos + len > data.size()) data.resize(f->pos + len);
    std::copy(tmp.begin(), tmp.end(),
              data.begin() + static_cast<std::ptrdiff_t>(f->pos));
    f->pos += len;
    return static_cast<std::int64_t>(len);
}

std::int64_t Vfs::lseek(int fd, std::int64_t offset, int whence) {
    File* f = get(fd);
    if (f == nullptr) return -sys::kEBADF;
    if (f->tty) return -sys::kESPIPE;

    std::int64_t base = 0;
    switch (whence) {
        case kSeekSet: base = 0; break;
        case kSeekCur: base = static_cast<std::int64_t>(f->pos); break;
        case kSeekEnd: {
            auto it = files_.find(f->path);
            base = (it == files_.end())
                       ? 0
                       : static_cast<std::int64_t>(it->second.size());
            break;
        }
        default: return -sys::kEINVAL;
    }
    const std::int64_t target = base + offset;
    if (target < 0) return -sys::kEINVAL;
    f->pos = static_cast<std::uint64_t>(target);
    return target;
}

std::optional<std::vector<std::byte>> Vfs::file_bytes(const std::string& path) const {
    auto it = files_.find(path);
    if (it == files_.end()) return std::nullopt;
    return it->second;
}

} // namespace kura::kernel
