#include "loader/elf.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n";   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// Hand-built synthetic ELF64 (all made-up values, no Sony code).
struct SyntheticElf {
    std::vector<std::byte> bytes;
    std::uint64_t entry = 0x400000;
    std::size_t code_off = 0x100;
};

SyntheticElf make_elf(const std::vector<std::uint8_t>& code) {
    SyntheticElf e;
    const std::size_t total = e.code_off + code.size();
    e.bytes.assign(total, std::byte{0});

    auto put = [&](std::size_t off, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i)
            e.bytes[off + static_cast<std::size_t>(i)] =
                static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    };

    // e_ident
    e.bytes[0] = std::byte{0x7F};
    e.bytes[1] = std::byte{'E'};
    e.bytes[2] = std::byte{'L'};
    e.bytes[3] = std::byte{'F'};
    e.bytes[4] = std::byte{2}; // ELFCLASS64
    e.bytes[5] = std::byte{1}; // ELFDATA2LSB
    e.bytes[6] = std::byte{1}; // EV_CURRENT
    put(16, 2, 2);             // ET_EXEC
    put(18, 0x3E, 2);          // EM_X86_64
    put(20, 1, 4);             // e_version
    put(24, e.entry, 8);       // e_entry
    put(32, 64, 8);            // e_phoff
    put(40, 0, 8);             // e_shoff
    put(48, 0, 4);             // e_flags
    put(52, 64, 2);            // e_ehsize
    put(54, 56, 2);            // e_phentsize
    put(56, 1, 2);             // e_phnum
    put(58, 0, 2);             // e_shentsize
    put(60, 0, 2);             // e_shnum
    put(62, 0, 2);             // e_shstrndx

    // program header at 64
    const std::size_t ph = 64;
    put(ph + 0, 1, 4);               // PT_LOAD
    put(ph + 4, 5, 4);               // PF_R | PF_X
    put(ph + 8, e.code_off, 8);      // p_offset
    put(ph + 16, e.entry, 8);        // p_vaddr
    put(ph + 24, e.entry, 8);        // p_paddr
    put(ph + 32, code.size(), 8);    // p_filesz
    put(ph + 40, 0x1000, 8);         // p_memsz
    put(ph + 48, 0x1000, 8);         // p_align

    for (std::size_t i = 0; i < code.size(); ++i)
        e.bytes[e.code_off + i] = static_cast<std::byte>(code[i]);
    return e;
}

void test_parse_valid() {
    // mov rax, 5 ; mov rbx, 7 ; add rax, rbx ; hlt
    const std::vector<std::uint8_t> code = {
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xD8,
        0xF4,
    };
    auto elf = make_elf(code);
    auto img = kura::loader::parse_elf64(elf.bytes.data(), elf.bytes.size());
    CHECK(img.has_value());
    if (!img) return;
    CHECK(img->entry == 0x400000);
    CHECK(img->segments.size() == 1);
    CHECK(img->segments[0].type == 1);
    CHECK(img->segments[0].vaddr == 0x400000);
    CHECK(img->segments[0].filesz == code.size());
    CHECK(img->segments[0].memsz == 0x1000);
}

void test_parse_rejects_garbage() {
    std::vector<std::byte> junk(64, std::byte{0x41});
    CHECK(!kura::loader::parse_elf64(junk.data(), junk.size()).has_value());

    auto elf = make_elf({0xF4});
    // truncate below ELF header size
    CHECK(!kura::loader::parse_elf64(elf.bytes.data(), 32).has_value());
    // wrong class (32-bit)
    elf.bytes[4] = std::byte{1};
    CHECK(!kura::loader::parse_elf64(elf.bytes.data(), elf.bytes.size()).has_value());
}

void test_parse_rejects_wrong_machine() {
    auto elf = make_elf({0xF4});
    elf.bytes[18] = std::byte{0xB7}; // not EM_X86_64
    CHECK(!kura::loader::parse_elf64(elf.bytes.data(), elf.bytes.size()).has_value());
}

void test_load_into_memory() {
    const std::vector<std::uint8_t> code = {0xF4};
    auto elf = make_elf(code);
    auto img = kura::loader::parse_elf64(elf.bytes.data(), elf.bytes.size());
    CHECK(img.has_value());
    if (!img) return;

    kura::GuestMemory mem;
    CHECK(kura::loader::load_into(*img, elf.bytes.data(), elf.bytes.size(), mem));
    CHECK(mem.is_mapped(0x400000, 0x1000));
    std::uint8_t byte = 0;
    CHECK(mem.read(0x400000, &byte, 1));
    CHECK(byte == 0xF4);
    // zero-fill beyond the file image
    std::uint8_t tail = 0x55;
    CHECK(mem.read(0x400000 + code.size(), &tail, 1));
    CHECK(tail == 0);
}

void test_load_rejects_truncated_file_image() {
    auto elf = make_elf({0xF4, 0xF4, 0xF4});
    auto img = kura::loader::parse_elf64(elf.bytes.data(), elf.bytes.size());
    CHECK(img.has_value());
    if (!img) return;
    kura::GuestMemory mem;
    // feed the loader a file truncated before p_offset+filesz
    CHECK(!kura::loader::load_into(*img, elf.bytes.data(), 0x100, mem));
}

} // namespace

int main() {
    test_parse_valid();
    test_parse_rejects_garbage();
    test_parse_rejects_wrong_machine();
    test_load_into_memory();
    test_load_rejects_truncated_file_image();
    if (g_failures == 0) std::cout << "test_elf: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
