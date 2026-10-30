// ---------------------------------------------------------------------------
// elf_loader — manual ELF shared-library loader (bypasses ld.so).
// ---------------------------------------------------------------------------
#ifdef __linux__

#include "elf_loader.h"
#include "injector.h"
#include "ptrace_ops.h"
#include "elf_parser.h"

#include <elf.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>

// ---- utilities ---------------------------------------------------------------

static std::string lastErrorStr() {
    return std::string(std::strerror(errno));
}

// Read an entire file into a byte vector.
static std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

// ---- symbol resolver ---------------------------------------------------------

// Caches symbol tables of already-loaded libraries.
struct SymResolver {
    struct LibInfo {
        uintptr_t base = 0;
        fs::path diskPath;
        // Owning copy of the ELF sections we need.  Must outlive pointer members.
        std::vector<uint8_t> elfData;
        // .dynsym / .dynstr / .gnu.hash / .hash pointers into elfData.
        const Elf64_Sym* symtab = nullptr;
        size_t symcount = 0;
        const char* strtab = nullptr;
        size_t strsz = 0;       // size of .dynstr — bounds-check all st_name accesses
        // For GNU hash lookup.
        const uint32_t* gnuHashData = nullptr;
        uint32_t gnuNBuckets = 0;
        uint32_t gnuSymndx = 0;
        const uint32_t* gnuBloom = nullptr;
        uint32_t gnuBloomShift = 0;
        uint32_t gnuBloomSize = 0;
    };

    std::vector<LibInfo> libs;

    bool init(int pid) {
        // Read target's memory map.
        std::vector<ptrace_ops::MapEntry> maps;
        if (!ptrace_ops::read_maps(pid, maps)) return false;

        for (const auto& m : maps) {
            if (m.perms.find('x') == std::string::npos) continue;
            if (m.path.empty()) continue;
            // Only process ELF shared objects.
            if (m.path.find(".so") == std::string::npos) continue;

            // Skip Qt plugins and other non-essential libraries.
            // We only need the core libraries that define our required symbols.
            if (m.path.find("/plugins/") != std::string::npos) continue;
            if (m.path.find("/dri/") != std::string::npos) continue;

            // Check if we've already processed this path.
            bool seen = false;
            for (const auto& l : libs) {
                if (l.diskPath == m.path) { seen = true; break; }
            }
            if (seen) continue;

            LibInfo li;
            li.base = m.start;
            li.diskPath = m.path;

            if (loadSymbols(li)) {
                libs.push_back(std::move(li));
            }
        }
        return !libs.empty();
    }

    // Resolve a symbol name (with optional @@version) to its runtime address.
    uintptr_t resolve(const char* name) {
        if (!name || !name[0]) return 0;
        std::string symName(name);
        auto atPos = symName.find("@@");
        if (atPos != std::string::npos) {
            symName = symName.substr(0, atPos);
        }

        // Use the well-tested on-disk ELF parser for all symbol lookups.
        for (const auto& li : libs) {
            std::string lookupName = symName;
            // Try with @@Qt_5 version tag (common for Qt symbols).
            uintptr_t off = findElfExportOffset(li.diskPath, lookupName);
            if (off == 0) {
                lookupName = symName + "@@Qt_5";
                off = findElfExportOffset(li.diskPath, lookupName);
            }
            if (off != 0) return li.base + off;
        }
        return 0;
    }

private:
    bool loadSymbols(LibInfo& li) {
        li.elfData = readFile(li.diskPath);
        if (li.elfData.size() < sizeof(Elf64_Ehdr)) return false;

        auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(li.elfData.data());
        if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return false;
        if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return false;

        // Find section headers.
        if (ehdr->e_shoff == 0 || ehdr->e_shentsize == 0) return false;

        auto* shdrs = reinterpret_cast<const Elf64_Shdr*>(
            li.elfData.data() + ehdr->e_shoff);
        const char* shstrtab = nullptr;
        if (ehdr->e_shstrndx < ehdr->e_shnum) {
            const auto& shstrSec = shdrs[ehdr->e_shstrndx];
            if (shstrSec.sh_offset < li.elfData.size()) {
                shstrtab = reinterpret_cast<const char*>(
                    li.elfData.data() + shstrSec.sh_offset);
            }
        }

        // Locate .dynsym, .dynstr, .gnu.hash, .hash.
        const Elf64_Shdr *dynsymSec = nullptr, *dynstrSec = nullptr;
        const Elf64_Shdr *gnuHashSec = nullptr, *hashSec = nullptr;

        for (Elf64_Half i = 0; i < ehdr->e_shnum; ++i) {
            if (!shstrtab) break;
            const char* secName = shstrtab + shdrs[i].sh_name;
            if (std::strcmp(secName, ".dynsym") == 0) dynsymSec = &shdrs[i];
            else if (std::strcmp(secName, ".dynstr") == 0) dynstrSec = &shdrs[i];
            else if (std::strcmp(secName, ".gnu.hash") == 0) gnuHashSec = &shdrs[i];
            else if (std::strcmp(secName, ".hash") == 0) hashSec = &shdrs[i];
        }

        if (dynsymSec && dynstrSec) {
            li.symtab = reinterpret_cast<const Elf64_Sym*>(
                li.elfData.data() + dynsymSec->sh_offset);
            li.symcount = dynsymSec->sh_size / sizeof(Elf64_Sym);
            li.strtab = reinterpret_cast<const char*>(
                li.elfData.data() + dynstrSec->sh_offset);
            li.strsz = dynstrSec->sh_size;
        }

        if (gnuHashSec) {
            li.gnuHashData = reinterpret_cast<const uint32_t*>(
                li.elfData.data() + gnuHashSec->sh_offset);
            if (gnuHashSec->sh_size >= 16) {
                li.gnuNBuckets  = li.gnuHashData[0];
                li.gnuSymndx    = li.gnuHashData[1];
                li.gnuBloomSize = li.gnuHashData[2];
                li.gnuBloomShift = li.gnuHashData[3];
                li.gnuBloom = li.gnuHashData + 4;
            }
        }
        (void)hashSec; // Not used; GNU hash is sufficient.

        return li.symtab != nullptr && li.strtab != nullptr;
    }

    static uint32_t gnuHash(const char* name) {
        uint32_t h = 5381;
        for (const uint8_t* p = reinterpret_cast<const uint8_t*>(name); *p; ++p)
            h = (h << 5) + h + *p;
        return h;
    }

    uintptr_t lookupGnuHash(const LibInfo& li, const char* name) {
        if (!li.gnuHashData || li.gnuNBuckets == 0 || !li.symtab) return 0;
        uint32_t h = gnuHash(name);

        // The GNU hash layout after the 4-word header:
        //   [bloom: gnuBloomSize words][buckets: gnuNBuckets words][chain: ...]
        // The chain occupies whatever remains.
        // Defensive: limit bloom size to something reasonable.
        uint32_t safeBloomSize = li.gnuBloomSize;
        if (safeBloomSize > 65536) return 0; // implausibly large
        if (!li.gnuBloom || li.gnuBloomShift >= 32) return 0;

        // Bloom filter.
        uint32_t bmIdx = (h / 32) % safeBloomSize;
        uint32_t bloomWord = li.gnuBloom[bmIdx];
        uint32_t bitmask = (1u << (h % 32)) |
                           (1u << ((h >> li.gnuBloomShift) % 32));
        if ((bloomWord & bitmask) != bitmask) return 0;

        const uint32_t* buckets = li.gnuBloom + safeBloomSize;
        uint32_t bucket = h % li.gnuNBuckets;
        uint32_t symndx = buckets[bucket];
        if (symndx < li.gnuSymndx) return 0;

        const uint32_t* chain = buckets + li.gnuNBuckets;
        uint32_t startIdx = symndx - li.gnuSymndx;

        for (uint32_t step = 0; step < 4096; ++step) {
            uint32_t ch = chain[startIdx + step];
            uint32_t si = symndx + step;
            if (si >= li.symcount) return 0; // out of bounds
            if ((ch & 1) && (ch | 1) == (h | 1)) {
                uint32_t snOff = li.symtab[si].st_name;
                if (snOff > 0 && snOff < li.strsz) {
                    const char* sn = li.strtab + snOff;
                    if (std::strcmp(sn, name) == 0 && li.symtab[si].st_value != 0)
                        return li.base + li.symtab[si].st_value;
                }
                return 0;
            }
            if (ch & 1) return 0;
        }
        return 0;
    }
};

// ---- ELF loader --------------------------------------------------------------

bool elfLoadLibrary(int pid, const fs::path& libPath, uintptr_t& outBase) {
    outBase = 0;

    // 1. Read the ELF file.
    auto elf = readFile(libPath);
    if (elf.size() < sizeof(Elf64_Ehdr))
        return false;

    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(elf.data());
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
        return false;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        return false;
    if (ehdr->e_type != ET_DYN)
        return false;

    // 2. Parse program headers.
    if (ehdr->e_phoff == 0 || ehdr->e_phentsize != sizeof(Elf64_Phdr))
        return false;

    auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(
        elf.data() + ehdr->e_phoff);

    // Find the PT_DYNAMIC segment and compute total memory needed.
    const Elf64_Phdr* dynamicPhdr = nullptr;
    uintptr_t minVaddr = UINTPTR_MAX;
    uintptr_t maxVaddr = 0;
    size_t totalMem = 0;

    // Pass 1: find ranges.
    for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
        const auto& ph = phdrs[i];
        if (ph.p_type == PT_LOAD) {
            if (ph.p_vaddr < minVaddr) minVaddr = ph.p_vaddr;
            uintptr_t end = ph.p_vaddr + ph.p_memsz;
            if (end > maxVaddr) maxVaddr = end;
        }
        if (ph.p_type == PT_DYNAMIC) {
            dynamicPhdr = &ph;
        }
    }

    if (maxVaddr == 0 || minVaddr == UINTPTR_MAX)
        return false;

    // Align to page boundaries.
    uintptr_t loadBase = minVaddr & ~static_cast<uintptr_t>(0xFFF);
    totalMem = ((maxVaddr + 0xFFF) & ~static_cast<size_t>(0xFFF)) - loadBase;

    // 3. Allocate memory in the target.
    //    Use two mappings: one RW for the combined data, then change
    //    permissions.  Simpler: use a single RWX mapping.
    uintptr_t targetBase = 0;
    if (!ptrace_ops::remote_syscall(pid, 9,   // __NR_mmap
            0, totalMem,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            static_cast<uintptr_t>(-1), 0,
            targetBase) || targetBase == 0 ||
        targetBase == static_cast<uintptr_t>(-1)) {
        return false;
    }

    outBase = targetBase;
    uintptr_t slide = targetBase - loadBase;

    // 4. Copy LOAD segments.
    for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
        const auto& ph = phdrs[i];
        if (ph.p_type != PT_LOAD) continue;

        uintptr_t segStart = targetBase + (ph.p_vaddr - loadBase);
        uintptr_t segStartPage = segStart & ~static_cast<uintptr_t>(0xFFF);
        size_t segOffset = segStart - segStartPage;
        size_t segCopySize = ph.p_filesz + segOffset;

        // Remap the region with file-backed data.
        if (ph.p_filesz > 0) {
            // Write the file data via process_vm_writev.
            struct iovec local_iov;
            local_iov.iov_base = const_cast<uint8_t*>(elf.data() + ph.p_offset);
            local_iov.iov_len = ph.p_filesz;

            struct iovec remote_iov;
            remote_iov.iov_base = reinterpret_cast<void*>(segStart);
            remote_iov.iov_len = ph.p_filesz;

            ssize_t n = process_vm_writev(pid, &local_iov, 1, &remote_iov, 1, 0);
            if (n != static_cast<ssize_t>(ph.p_filesz)) {
                std::fprintf(stderr, "[elfLoad] WARNING: writev wrote %zd/%lu bytes\n",
                             n, static_cast<unsigned long>(ph.p_filesz));
            }
        }

        // Zero-fill .bss portion (p_memsz > p_filesz).
        if (ph.p_memsz > ph.p_filesz) {
            uintptr_t bssStart = segStart + ph.p_filesz;
            size_t bssLen = ph.p_memsz - ph.p_filesz;
            // Write zeros via process_vm_writev.
            std::vector<uint8_t> zeros(bssLen, 0);
            struct iovec local_iov;
            local_iov.iov_base = zeros.data();
            local_iov.iov_len = bssLen;
            struct iovec remote_iov;
            remote_iov.iov_base = reinterpret_cast<void*>(bssStart);
            remote_iov.iov_len = bssLen;
            process_vm_writev(pid, &local_iov, 1, &remote_iov, 1, 0);
        }
    }

    // 5. Apply relocations.
    if (!dynamicPhdr) goto set_perms;

    {
        // Read the .dynamic section from the target (we just copied it there).
        const auto& dynPhdr = *dynamicPhdr;
        size_t dynSize = dynPhdr.p_filesz;
        std::vector<Elf64_Dyn> dynEntries(dynSize / sizeof(Elf64_Dyn));
        ptrace_ops::read_mem(pid, targetBase + dynPhdr.p_vaddr,
                              dynEntries.data(), dynSize);

        uintptr_t relaOff = 0, relaSz = 0;
        uintptr_t jmprelOff = 0, pltRelSz = 0;
        uintptr_t symOff = 0, strOff = 0, strSz = 0;

        for (const auto& d : dynEntries) {
            if (d.d_tag == DT_NULL) break;
            if (d.d_tag == DT_RELA)      relaOff    = d.d_un.d_ptr;
            if (d.d_tag == DT_RELASZ)     relaSz     = d.d_un.d_val;
            if (d.d_tag == DT_JMPREL)     jmprelOff  = d.d_un.d_ptr;
            if (d.d_tag == DT_PLTRELSZ)   pltRelSz   = d.d_un.d_val;
            if (d.d_tag == DT_SYMTAB)     symOff     = d.d_un.d_ptr;
            if (d.d_tag == DT_STRTAB)     strOff     = d.d_un.d_ptr;
            if (d.d_tag == DT_STRSZ)      strSz      = d.d_un.d_val;
        }

        bool haveRelocs = (relaOff != 0 && relaSz != 0);
        bool havePltRelocs = (jmprelOff != 0 && pltRelSz != 0);

        // Read the string table from the target (for symbol names).
        std::vector<char> strBuf;
        const char* strtab = nullptr;
        if (strOff != 0 && strSz > 0 && strSz < 16 * 1024 * 1024) {
            strBuf.resize(strSz);
            ptrace_ops::read_mem(pid, targetBase + strOff,
                                  strBuf.data(), strSz);
            strtab = strBuf.data();
        }

        // Read the symbol table from the target.
        std::vector<Elf64_Sym> symBuf;
        const Elf64_Sym* symtab = nullptr;
        size_t symCount = 0;
        if (symOff != 0) {
            // Estimate symbol table size: it ends where strtab begins.
            size_t symSize = (strOff > symOff) ? (strOff - symOff) : 65536;
            symBuf.resize(symSize / sizeof(Elf64_Sym));
            ptrace_ops::read_mem(pid, targetBase + symOff,
                                  symBuf.data(), symSize);
            symtab = symBuf.data();
            symCount = symBuf.size();
        }

        // Build symbol resolver for external symbols.
        SymResolver resolver;
        bool haveResolver = resolver.init(pid);

        // Known-safe symbols that are optional and may legitimately resolve
        // to zero: GCC transactional-memory weak symbols, glibc's
        // __gmon_start__, and the glibc >= 2.32 single-threaded optimisation
        // flag (older glibc doesn't export it).
        auto isOptionalSymbol = [](const char* name) -> bool {
            if (!name) return false;
            if (std::strncmp(name, "_ITM_", 5) == 0) return true;
            if (std::strcmp(name, "__gmon_start__") == 0) return true;
            if (std::strcmp(name, "__libc_single_threaded") == 0) return true;
            return false;
        };

        // Helper to resolve a symbol value, trying the internal symtab first
        // (st_value != 0 means it's defined in our own library), then the
        // external resolver for already-loaded shared libraries.
        auto resolveSym = [&](uint32_t symIdx) -> uintptr_t {
            if (!symtab || !strtab) return 0;
            if (symIdx == 0 || symIdx >= symCount) return 0;
            if (symtab[symIdx].st_value != 0)
                return targetBase + symtab[symIdx].st_value;
            uint32_t stName = symtab[symIdx].st_name;
            if (stName == 0 || stName >= strSz) return 0;
            const char* symName = strtab + stName;
            if (isOptionalSymbol(symName)) return 0;
            if (!haveResolver) return 0;
            return resolver.resolve(symName);
        };

        size_t relocTotal = 0, relocDone = 0;

        // ---- .rela.dyn (R_X86_64_RELATIVE / GLOB_DAT / COPY) -------------------
        if (haveRelocs) {
            size_t relocCount = relaSz / sizeof(Elf64_Rela);
            std::vector<Elf64_Rela> relocs(relocCount);
            ptrace_ops::read_mem(pid, targetBase + relaOff,
                                  relocs.data(), relaSz);

            for (size_t i = 0; i < relocCount; ++i) {
                const auto& r = relocs[i];
                uintptr_t relocAddr = targetBase + r.r_offset;
                uint32_t rtype = ELF64_R_TYPE(r.r_info);

                switch (rtype) {
                case R_X86_64_NONE:
                    break;

                case R_X86_64_RELATIVE: {
                    uintptr_t newVal = targetBase + r.r_addend;
                    ptrace_ops::write_word(pid, relocAddr, newVal);
                    relocDone++;
                    relocTotal++;
                    break;
                }

                case R_X86_64_GLOB_DAT: {
                    uintptr_t symVal = resolveSym(ELF64_R_SYM(r.r_info));
                    uintptr_t newVal = symVal + r.r_addend;
                    ptrace_ops::write_word(pid, relocAddr, newVal);
                    relocTotal++;
                    relocDone++;  // GLOB_DAT is handled regardless — optional
                                  // symbols safely resolve to zero
                    break;
                }

                case R_X86_64_COPY: {
                    uintptr_t srcAddr = 0;
                    size_t copySize = 0;
                    uint32_t symIdx = ELF64_R_SYM(r.r_info);
                    if (symIdx > 0 && symIdx < symCount) {
                        const char* symName = strtab + symtab[symIdx].st_name;
                        srcAddr = resolver.resolve(symName);
                        copySize = symtab[symIdx].st_size;
                    }
                    relocTotal++;
                    if (srcAddr != 0 && copySize > 0) {
                        std::vector<uint8_t> copyBuf(copySize);
                        ptrace_ops::read_mem(pid, srcAddr, copyBuf.data(), copySize);
                        ptrace_ops::write_mem(pid, relocAddr, copyBuf.data(), copySize);
                    }
                    relocDone++;
                    break;
                }

                default:
                    break;
                }
            }
        }

        // ---- .rela.plt (R_X86_64_JUMP_SLOT) ----------------------------------
        if (havePltRelocs) {
            size_t pltCount = pltRelSz / sizeof(Elf64_Rela);
            std::vector<Elf64_Rela> pltRelocs(pltCount);
            ptrace_ops::read_mem(pid, targetBase + jmprelOff,
                                  pltRelocs.data(), pltRelSz);

            for (size_t i = 0; i < pltCount; ++i) {
                const auto& r = pltRelocs[i];
                uintptr_t relocAddr = targetBase + r.r_offset;
                uint32_t rtype = ELF64_R_TYPE(r.r_info);

                if (rtype == R_X86_64_JUMP_SLOT) {
                    uintptr_t symVal = resolveSym(ELF64_R_SYM(r.r_info));
                    uintptr_t newVal = symVal + r.r_addend;
                    ptrace_ops::write_word(pid, relocAddr, newVal);
                    relocTotal++;
                    relocDone++;
                    // NOTE: optional symbols (ITM, gmon, etc.) resolve to 0
                    // which is safe — their PLT entries are never called.
                }
                // R_X86_64_IRELATIVE can also appear in .rela.plt (glibc
                // IFUNC resolvers); we don't support these, but they should
                // not appear in a non-PIE shared library with lazy binding.
            }
        }

        if (relocTotal > 0) {
            std::fprintf(stderr,
                "[elfLoad] %zu/%zu relocations processed\n",
                relocDone, relocTotal);
        }
    }

set_perms:
    // 6. Set final permissions on the mapped region.
    //    Use mprotect via remote_syscall.
    {
        int prot = PROT_READ;
        for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
            const auto& ph = phdrs[i];
            if (ph.p_type != PT_LOAD) continue;
            int segProt = 0;
            if (ph.p_flags & PF_R) segProt |= PROT_READ;
            if (ph.p_flags & PF_W) segProt |= PROT_WRITE;
            if (ph.p_flags & PF_X) segProt |= PROT_EXEC;

            uintptr_t segStart = targetBase + (ph.p_vaddr - loadBase);
            segStart &= ~static_cast<uintptr_t>(0xFFF);
            uintptr_t segEnd = targetBase + ph.p_vaddr + ph.p_memsz - loadBase;
            segEnd = (segEnd + 0xFFF) & ~static_cast<size_t>(0xFFF);
            size_t segSize = segEnd - segStart;

            uintptr_t dmy = 0;
            ptrace_ops::remote_syscall(pid, 10,  // __NR_mprotect
                segStart, segSize, segProt, 0, 0, 0, dmy);
        }
    }

    return true;
}

#endif // __linux__
