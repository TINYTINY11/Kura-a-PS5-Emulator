#include "kernel/vfs.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <string_view>

#include "common/log.hpp"
#include "kernel/syscalls.hpp"

namespace kura::kernel {

namespace {
void stamp(std::int64_t& sec, std::int32_t& nsec) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    nsec = static_cast<std::int32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() -
        sec * 1000000000);
}

// A host file is materialized whole before a guest can read it — refuse
// anything bigger so one huge file can't balloon the process.
constexpr std::uint64_t kMaxHostLoad = 512ull * 1024 * 1024;

// file_time_type -> system-clock timespec without depending on the file
// clock exposing to_sys (MSVC's doesn't): convert the file's age relative
// to "now" across clock domains. Sub-second drift between the two now()
// samples is invisible at stat resolution.
void file_mtime(const std::filesystem::path& p, std::int64_t& sec,
                std::int32_t& nsec) {
    std::error_code ec;
    const auto ftp = std::filesystem::last_write_time(p, ec);
    if (ec) return;
    const auto file_now = std::filesystem::file_time_type::clock::now();
    const auto sys_now = std::chrono::system_clock::now();
    const auto tp =
        sys_now -
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            file_now - ftp);
    sec = std::chrono::duration_cast<std::chrono::seconds>(
              tp.time_since_epoch())
              .count();
    nsec = static_cast<std::int32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            tp.time_since_epoch())
            .count() -
        sec * 1000000000);
}
} // namespace

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
    if (files_.find(path) != files_.end()) return true;
    if (host_root_.empty()) return false;
    std::filesystem::path target;
    if (!host_resolve(path, target)) return false;
    std::error_code ec;
    const bool ex = std::filesystem::exists(target, ec);
    return !ec && ex;
}

// ------------------------------------------------------------- host bridge ----

bool Vfs::mount_host(const std::string& abs_root) {
    if (abs_root.empty()) {
        host_root_.clear();
        return true;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(abs_root, ec) || ec) return false;
    std::filesystem::path canon =
        std::filesystem::weakly_canonical(abs_root, ec);
    host_root_ = ec ? std::filesystem::path(abs_root) : canon;
    log::info("kernel.vfs", "host bridge mounted at ", host_root_.string());
    return true;
}

bool Vfs::host_resolve(const std::string& guest,
                       std::filesystem::path& out) const {
    if (host_root_.empty()) return false;
    // Reject escape shapes up front: drive letters/ADS (''), backslashes
    // (never a separator in a guest path), NUL.
    if (guest.find(':') != std::string::npos) return false;
    if (guest.find('\\') != std::string::npos) return false;
    if (guest.find('\0') != std::string::npos) return false;

    std::filesystem::path cand;
    std::string comp;
    const auto flush = [&]() -> bool {
        if (comp.empty() || comp == ".") { comp.clear(); return true; }
        if (comp == "..") return false; // never climb, at any depth
        cand /= comp;
        comp.clear();
        return true;
    };
    for (const char c : guest) {
        if (c == '/') {
            if (!flush()) return false;
        } else {
            comp.push_back(c);
        }
    }
    if (!flush()) return false;

    // Canonicalize and verify the result still lives under the root —
    // this is what stops a host symlink inside the tree from leading out.
    std::error_code ec;
    const auto root_c = std::filesystem::weakly_canonical(host_root_, ec);
    if (ec) return false;
    const auto cand_c = std::filesystem::weakly_canonical(root_c / cand, ec);
    if (ec) return false;
    auto rit = root_c.begin();
    auto cit = cand_c.begin();
    for (; rit != root_c.end(); ++rit, ++cit) {
        if (cit == cand_c.end() || *cit != *rit) return false;
    }
    out = cand_c;
    return true;
}

Vfs::HostResult Vfs::host_open_into(const std::string& path) {
    if (host_root_.empty()) return HostResult::Miss;
    std::filesystem::path target;
    if (!host_resolve(path, target)) return HostResult::Escape;
    std::error_code ec;
    if (std::filesystem::is_directory(target, ec)) return HostResult::IsDir;
    if (ec || !std::filesystem::is_regular_file(target, ec))
        return HostResult::Miss;
    const auto sz = std::filesystem::file_size(target, ec);
    if (ec) return HostResult::Miss;
    if (sz > kMaxHostLoad) return HostResult::TooBig;

    std::ifstream in(target, std::ios::binary);
    if (!in) return HostResult::Miss;
    Node n;
    n.ino = static_cast<std::uint64_t>(std::hash<std::string>{}(path));
    n.data.resize(static_cast<std::size_t>(sz));
    if (sz > 0) {
        in.read(reinterpret_cast<char*>(n.data.data()),
                static_cast<std::streamsize>(sz));
        if (in.gcount() != static_cast<std::streamsize>(sz))
            return HostResult::Miss;
    }
    file_mtime(target, n.mtime_sec, n.mtime_nsec);
    log::debug("kernel.vfs", "host bridge materialized ", path, " (", sz,
               " bytes, copy-on-open)");
    files_.emplace(path, std::move(n));
    return HostResult::Loaded;
}

bool Vfs::host_stat(const std::string& path, StatInfo& out) const {
    if (host_root_.empty()) return false;
    std::filesystem::path target;
    if (!host_resolve(path, target)) return false;
    std::error_code ec;
    const bool isdir = std::filesystem::is_directory(target, ec);
    if (ec) return false;
    if (!isdir && !std::filesystem::is_regular_file(target, ec)) return false;

    out = StatInfo{};
    // Stable stand-in for the real inode (not portably available).
    out.ino = static_cast<std::uint64_t>(std::hash<std::string>{}(path));
    out.is_dir = isdir;
    out.mode_perm = isdir ? 0755 : 0644; // predicted
    if (!isdir) {
        const auto sz = std::filesystem::file_size(target, ec);
        if (!ec) out.size = sz;
        file_mtime(target, out.mtime_sec, out.mtime_nsec);
    }
    return true;
}

int Vfs::open(const std::string& path, std::uint32_t oflags) {
    if (path.empty()) return -sys::kENOENT;
    const bool creat = (oflags & kOCreat) != 0;
    const bool excl = (oflags & kOExcl) != 0;
    const bool acc_wr = (oflags & kOAccmode) != kORead;

    auto it = files_.find(path);
    if (it == files_.end()) {
        // Host bridge: materialize first (copy-on-open — writes below
        // then touch only the store, never the host file).
        if (!host_root_.empty()) {
            switch (host_open_into(path)) {
            case HostResult::Escape:
                return -sys::kENOENT; // sandbox rejection looks like a miss
            case HostResult::IsDir:
                return -sys::kEISDIR;
            case HostResult::TooBig:
                return -sys::kEFBIG;
            case HostResult::Miss:
            case HostResult::Loaded:
                break;
            }
            it = files_.find(path);
        }
    }
    if (it != files_.end()) {
        if (creat && excl) return -sys::kEEXIST;
        if (acc_wr && (oflags & kOTrunc)) {
            it->second.data.clear();
            stamp(it->second.mtime_sec, it->second.mtime_nsec);
        }
    } else {
        if (!creat) return -sys::kENOENT;
        Node n;
        n.ino = next_ino_++;
        stamp(n.mtime_sec, n.mtime_nsec);
        it = files_.emplace(path, std::move(n)).first;
    }

    File f;
    f.path = path;
    f.flags = oflags;
    f.pos = ((oflags & kOAppend) != 0) ? it->second.data.size() : 0;
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
    auto& data = it->second.data;

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
    auto& data = it->second.data;
    if (f->pos + len > data.size()) data.resize(f->pos + len);
    std::copy(tmp.begin(), tmp.end(),
              data.begin() + static_cast<std::ptrdiff_t>(f->pos));
    f->pos += len;
    stamp(it->second.mtime_sec, it->second.mtime_nsec);
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
                       : static_cast<std::int64_t>(it->second.data.size());
            break;
        }
        default: return -sys::kEINVAL;
    }
    const std::int64_t target = base + offset;
    if (target < 0) return -sys::kEINVAL;
    f->pos = static_cast<std::uint64_t>(target);
    return target;
}

Vfs::StatInfo Vfs::info_of(const Node& n) const {
    StatInfo si;
    si.ino = n.ino;
    si.size = n.data.size();
    si.mtime_sec = n.mtime_sec;
    si.mtime_nsec = n.mtime_nsec;
    si.mode_perm = 0644; // predicted default: regular file, rw-r--r--
    return si;
}

bool Vfs::stat_path(const std::string& path, StatInfo& out) const {
    auto it = files_.find(path);
    if (it != files_.end()) {
        out = info_of(it->second);
        return true;
    }
    return host_stat(path, out); // no side effects — stat never materializes
}

bool Vfs::stat_fd(int fd, StatInfo& out) const {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return false;
    const File& f = it->second;
    if (f.path.rfind("/dev/", 0) == 0) { // stdin/stdout/stderr are char devs
        out = StatInfo{};
        out.is_char = true;
        out.mode_perm = 0666; // rw-rw-rw- — predicted
        return true;
    }
    auto n = files_.find(f.path);
    if (n == files_.end()) return false;
    out = info_of(n->second);
    return true;
}

std::optional<std::vector<std::byte>> Vfs::file_bytes(const std::string& path) const {
    auto it = files_.find(path);
    if (it == files_.end()) return std::nullopt;
    return it->second.data;
}

} // namespace kura::kernel
