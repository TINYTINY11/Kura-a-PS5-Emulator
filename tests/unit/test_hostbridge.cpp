// M3 stage 4 host-bridge test: a real directory tree is mounted into the
// Vfs, read through guest file operations, and — critically — the host
// tree must be byte-for-byte untouched after guest writes (copy-on-open),
// with every escape attempt rejected.

#include "kernel/syscalls.hpp"
#include "kernel/vfs.hpp"
#include "memory/guest_memory.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace fs = std::filesystem;
using kura::GuestMemory;
using kura::kernel::Vfs;

constexpr std::uint64_t kScratch = 0x100000;

fs::path g_root;   // mounted tree
fs::path g_outside; // a file that must never be reachable

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

void setup_tree() {
    g_root = fs::temp_directory_path() / "kura_hostbridge_root";
    fs::remove_all(g_root);
    fs::create_directories(g_root / "sub");
    std::ofstream(g_root / "readme.txt", std::ios::binary) << "host content";
    std::ofstream(g_root / "sub" / "nested.dat", std::ios::binary) << "xyz";

    g_outside = fs::temp_directory_path() / "kura_hostbridge_outside.txt";
    std::ofstream(g_outside, std::ios::binary) << "secret";
}

void teardown_tree() {
    fs::remove_all(g_root);
    fs::remove(g_outside);
}

void test_mount_rejects_bad_roots() {
    Vfs v;
    CHECK(!v.mount_host((g_root / "does_not_exist").string()));
    CHECK(!v.host_mounted());
    // unmounted bridge: nothing resolves
    CHECK(v.open("/readme.txt", kura::kernel::kORead) ==
          -kura::kernel::sys::kENOENT);
}

void test_read_host_file() {
    Vfs v;
    CHECK(v.mount_host(g_root.string()));
    CHECK(v.host_mounted());

    GuestMemory mem;
    CHECK(mem.map(kScratch, 0x1000));

    const int fd = v.open("/readme.txt", kura::kernel::kORead);
    CHECK(fd >= 3);
    char buf[32] = {};
    CHECK(v.read(fd, mem, kScratch, sizeof(buf)) == 12);
    CHECK(mem.read(kScratch, buf, 12));
    CHECK(std::string(buf, 12) == "host content");

    // nested path
    const int fd2 = v.open("/sub/nested.dat", kura::kernel::kORead);
    CHECK(fd2 >= 3);
    CHECK(v.read(fd2, mem, kScratch, 8) == 3);
    CHECK(mem.read(kScratch, buf, 3));
    CHECK(std::string(buf, 3) == "xyz");
}

void test_stat_without_open() {
    Vfs v;
    CHECK(v.mount_host(g_root.string()));

    Vfs::StatInfo si;
    CHECK(v.stat_path("/readme.txt", si));
    CHECK(si.size == 12);
    CHECK(!si.is_dir);
    CHECK(si.ino > 0);
    CHECK(si.mtime_sec > 1'700'000'000);
    CHECK(si.mode_perm == 0644);

    CHECK(v.stat_path("/sub", si));
    CHECK(si.is_dir);
    CHECK(si.mode_perm == 0755);

    CHECK(!v.stat_path("/no_such_file", si));
    // stat must NOT materialize into the store (no side effects)
    CHECK(v.file_bytes("/readme.txt") == std::nullopt);
}

void test_write_never_touches_host() {
    Vfs v;
    CHECK(v.mount_host(g_root.string()));

    // open for write + truncate: copy-on-open, then store-only mutation
    const int fd = v.open("/readme.txt",
                          kura::kernel::kOWrite | kura::kernel::kOTrunc);
    CHECK(fd >= 3);
    GuestMemory mem;
    CHECK(mem.map(kScratch, 0x1000));
    CHECK(mem.write(kScratch, "kura!", 5));
    CHECK(v.write(fd, mem, kScratch, 5) == 5);
    CHECK(v.close(fd) == 0);

    // guest now reads the modified copy...
    const int fd2 = v.open("/readme.txt", kura::kernel::kORead);
    char buf[16] = {};
    CHECK(v.read(fd2, mem, kScratch, sizeof(buf)) == 5);
    CHECK(mem.read(kScratch, buf, 5));
    CHECK(std::string(buf, 5) == "kura!");

    // ...but the REAL host file is byte-for-byte unchanged
    CHECK(read_all(g_root / "readme.txt") == "host content");
}

void test_escapes_rejected() {
    Vfs v;
    CHECK(v.mount_host(g_root.string()));

    // dot-dot at any depth
    CHECK(v.open("/../kura_hostbridge_outside.txt", kura::kernel::kORead) ==
          -kura::kernel::sys::kENOENT);
    CHECK(v.open("/sub/../../kura_hostbridge_outside.txt",
                 kura::kernel::kORead) == -kura::kernel::sys::kENOENT);
    Vfs::StatInfo si;
    CHECK(!v.stat_path("/../kura_hostbridge_outside.txt", si));

    // drive letters and backslashes are not guest-path syntax
    CHECK(v.open("C:/Windows/win.ini", kura::kernel::kORead) ==
          -kura::kernel::sys::kENOENT);
    CHECK(v.open("..\\..\\evil", kura::kernel::kORead) ==
          -kura::kernel::sys::kENOENT);

    // O_CREAT on an escaping path must not create anything either —
    // not in the store, and certainly not on disk next to the root
    CHECK(v.open("/../escaped.txt",
                 kura::kernel::kORead | kura::kernel::kOCreat) ==
          -kura::kernel::sys::kENOENT);
    CHECK(!fs::exists(g_root.parent_path() / "escaped.txt"));

    // the outside file itself was never opened/read by any of this
    CHECK(read_all(g_outside) == "secret");
}

void test_dirs_are_eisdir() {
    Vfs v;
    CHECK(v.mount_host(g_root.string()));
    CHECK(v.open("/sub", kura::kernel::kORead) ==
          -kura::kernel::sys::kEISDIR);
    CHECK(v.open("/", kura::kernel::kORead) ==
          -kura::kernel::sys::kEISDIR);
}

void test_store_shadows_host() {
    Vfs v;
    CHECK(v.mount_host(g_root.string()));

    // a file created in the store under an existing host path wins
    const int fd =
        v.open("/readme.txt", kura::kernel::kOCreat | kura::kernel::kOExcl);
    // host file exists (materializes first) -> O_EXCL correctly says EEXIST
    CHECK(fd == -kura::kernel::sys::kEEXIST);

    // O_CREAT on a brand-new path stays in the store
    const int fd2 =
        v.open("/fresh.txt", kura::kernel::kOCreat | kura::kernel::kOWrite);
    CHECK(fd2 >= 3);
    CHECK(v.close(fd2) == 0);
    CHECK(!fs::exists(g_root / "fresh.txt")); // store-only, host untouched
}

} // namespace

int main() {
    setup_tree();
    test_mount_rejects_bad_roots();
    test_read_host_file();
    test_stat_without_open();
    test_write_never_touches_host();
    test_escapes_rejected();
    test_dirs_are_eisdir();
    test_store_shadows_host();
    teardown_tree();
    if (g_failures == 0) std::cout << "test_hostbridge: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
