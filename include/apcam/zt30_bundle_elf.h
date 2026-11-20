#pragma once
#include "zt30_slots.h"
#include <elf.h>

namespace apcam_zt30 {
// Full transitive/version/ARMv7 qualification is performed by the builder using
// check_zt30_elf.py. The device checks the basic ABI without running candidate
// code, and separately pins the retained libraries to the qualified runtime.
inline bool basic_elf(const std::string &path, bool shared, std::string &error)
{
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st{}; Elf32_Ehdr h{};
    auto read_at = [&](void *p, size_t size, uint64_t offset) {
        if (offset > uint64_t(st.st_size) || size > uint64_t(st.st_size)-offset) return false;
        size_t used = 0;
        while (used < size) {
            ssize_t n = pread(fd, static_cast<char *>(p)+used, size-used, off_t(offset+used));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            used += size_t(n);
        }
        return true;
    };
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= off_t(sizeof(h)) &&
        read_at(&h, sizeof(h), 0) && !memcmp(h.e_ident, ELFMAG, SELFMAG) &&
        h.e_ident[EI_CLASS] == ELFCLASS32 && h.e_ident[EI_DATA] == ELFDATA2LSB &&
        h.e_ident[EI_VERSION] == EV_CURRENT && h.e_version == EV_CURRENT && h.e_machine == EM_ARM &&
        (h.e_type == ET_DYN || (!shared && h.e_type == ET_EXEC)) &&
        (h.e_flags & EF_ARM_EABIMASK) == EF_ARM_EABI_VER5 && (h.e_flags & EF_ARM_ABI_FLOAT_HARD) &&
        !(h.e_flags & EF_ARM_ABI_FLOAT_SOFT) && h.e_ehsize == sizeof(h) &&
        h.e_phentsize == sizeof(Elf32_Phdr) && h.e_phnum && h.e_phnum <= 128;
    unsigned interpreters = 0;
    if (ok && (uint64_t(h.e_phoff)+uint64_t(h.e_phnum)*sizeof(Elf32_Phdr) > uint64_t(st.st_size))) ok = false;
    for (unsigned i=0; ok && i<h.e_phnum; ++i) {
        Elf32_Phdr ph{};
        ok = read_at(&ph, sizeof(ph), uint64_t(h.e_phoff)+i*sizeof(ph)) &&
            ph.p_offset <= uint64_t(st.st_size) && ph.p_filesz <= uint64_t(st.st_size)-ph.p_offset;
        if (ok && ph.p_type == PT_INTERP) {
            static const char expected[] = "/lib/ld-linux-armhf.so.3";
            char interpreter[sizeof(expected)];
            ok = ++interpreters == 1 && ph.p_filesz == sizeof(expected) &&
                read_at(interpreter, sizeof(interpreter), ph.p_offset) && !memcmp(interpreter, expected, sizeof(expected));
        }
    }
    ok = ok && interpreters == (shared ? 0U : 1U);
    if (fd >= 0) close(fd);
    return ok || fail(error, "Invalid basic ARM hard-float ELF ABI: " + path);
}
} // namespace apcam_zt30
