// ---------------------------------------------------------------------------
// elf_parser — Linux ELF shared-library dependency and symbol parser.
// ---------------------------------------------------------------------------
#ifdef __linux__

#include "elf_parser.h"

#include <cstring>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>
#include <algorithm>
#include <elf.h>

// ---- Helpers ---------------------------------------------------------------

namespace {

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(data.data()), sz);
    if (!f) return {};
    return data;
}

bool isElf(const std::vector<uint8_t>& data) {
    return data.size() >= SELFMAG &&
           std::memcmp(data.data(), ELFMAG, SELFMAG) == 0;
}

bool is64(const std::vector<uint8_t>& data) {
    return data.size() > EI_CLASS && data[EI_CLASS] == ELFCLASS64;
}

// Get section header string table.
const char* shStrTab(const std::vector<uint8_t>& data,
                     const Elf64_Ehdr* ehdr) {
    if (ehdr->e_shstrndx == SHN_UNDEF) return nullptr;
    uint64_t shoff = ehdr->e_shoff;
    uint64_t idx = ehdr->e_shstrndx;
    if (shoff + (idx + 1) * ehdr->e_shentsize > data.size()) return nullptr;

    auto* shdr = reinterpret_cast<const Elf64_Shdr*>(
        data.data() + shoff + idx * ehdr->e_shentsize);
    if (shdr->sh_offset + shdr->sh_size > data.size()) return nullptr;
    return reinterpret_cast<const char*>(data.data() + shdr->sh_offset);
}

// Find a section by name.  Returns nullptr if not found.
const Elf64_Shdr* findSection(const std::vector<uint8_t>& data,
                              const Elf64_Ehdr* ehdr,
                              const char* name) {
    const char* strTab = shStrTab(data, ehdr);
    if (!strTab) return nullptr;

    uint64_t shoff = ehdr->e_shoff;
    for (int i = 0; i < ehdr->e_shnum; ++i) {
        auto* shdr = reinterpret_cast<const Elf64_Shdr*>(
            data.data() + shoff + i * ehdr->e_shentsize);
        if (shdr->sh_name == 0) continue;
        const char* sname = strTab + shdr->sh_name;
        if (std::strcmp(sname, name) == 0) return shdr;
    }
    return nullptr;
}

// Read an unsigned LEB128 value from a buffer.
uint64_t readULEB128(const uint8_t*& p, const uint8_t* end) {
    uint64_t result = 0;
    int shift = 0;
    while (p < end) {
        uint8_t byte = *p++;
        result |= (static_cast<uint64_t>(byte & 0x7F)) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    return result;
}

} // anonymous namespace

// ---- Dependency parsing via .dynamic / DT_NEEDED ---------------------------

std::vector<std::string> parseElfDependencies(const fs::path& libPath) {
    std::vector<std::string> deps;
    auto data = readFile(libPath);
    if (!isElf(data)) return deps;
    if (!is64(data)) {
        // 32-bit ELF not supported yet; return empty.
        return deps;
    }

    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());

    // Find the .dynamic section.
    const Elf64_Shdr* dynSec = findSection(data, ehdr, ".dynamic");
    if (!dynSec) {
        // .dynamic is also accessible via program headers (PT_DYNAMIC).
        // Try program headers for the dynamic segment.
        uint64_t phoff = ehdr->e_phoff;
        uint64_t dynamicOff = 0;
        uint64_t dynamicSize = 0;
        for (int i = 0; i < ehdr->e_phnum; ++i) {
            auto* phdr = reinterpret_cast<const Elf64_Phdr*>(
                data.data() + phoff + i * ehdr->e_phentsize);
            if (phdr->p_type == PT_DYNAMIC) {
                dynamicOff = phdr->p_offset;
                dynamicSize = phdr->p_filesz;
                break;
            }
        }
        if (dynamicOff == 0 || dynamicSize == 0) return deps;
        // Use dynamicOff/dynamicSize directly.
        const Elf64_Dyn* dyn = reinterpret_cast<const Elf64_Dyn*>(
            data.data() + dynamicOff);
        size_t dynCount = dynamicSize / sizeof(Elf64_Dyn);

        // Find the string table (DT_STRTAB) and DT_NEEDED entries.
        const char* dynStr = nullptr;
        for (size_t j = 0; j < dynCount; ++j) {
            if (dyn[j].d_tag == DT_STRTAB) {
                uint64_t stroff = dyn[j].d_un.d_val;
                // d_val is a virtual address; we need file offset.
                // For simple .so files, sections are at the same offset.
                // We search section headers for a match.
                uint64_t shoff = ehdr->e_shoff;
                for (int k = 0; k < ehdr->e_shnum; ++k) {
                    auto* sh = reinterpret_cast<const Elf64_Shdr*>(
                        data.data() + shoff + k * ehdr->e_shentsize);
                    if (sh->sh_addr <= stroff &&
                        stroff < sh->sh_addr + sh->sh_size) {
                        dynStr = reinterpret_cast<const char*>(
                            data.data() + sh->sh_offset +
                            (stroff - sh->sh_addr));
                        break;
                    }
                }
            }
        }
        if (!dynStr) return deps;

        for (size_t j = 0; j < dynCount; ++j) {
            if (dyn[j].d_tag == DT_NEEDED) {
                uint64_t strOff = dyn[j].d_un.d_val;
                deps.emplace_back(dynStr + strOff);
            } else if (dyn[j].d_tag == DT_NULL) {
                break;
            }
        }
    } else {
        // Use the .dynamic section.
        const Elf64_Shdr* dynStrSec = findSection(data, ehdr, ".dynstr");
        if (!dynStrSec) return deps;

        const char* dynStr = reinterpret_cast<const char*>(
            data.data() + dynStrSec->sh_offset);
        auto* dyn = reinterpret_cast<const Elf64_Dyn*>(
            data.data() + dynSec->sh_offset);
        size_t dynCount = dynSec->sh_size / sizeof(Elf64_Dyn);
        for (size_t j = 0; j < dynCount; ++j) {
            if (dyn[j].d_tag == DT_NEEDED) {
                deps.emplace_back(dynStr + dyn[j].d_un.d_val);
            } else if (dyn[j].d_tag == DT_NULL) {
                break;
            }
        }
    }

    // Deduplicate.
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    return deps;
}

// ---- Dependency closure ----------------------------------------------------

std::vector<fs::path> resolveElfDependencyClosure(
    const fs::path& libPath,
    const std::vector<fs::path>& searchDirs)
{
    std::vector<fs::path> result;
    std::vector<fs::path> queue;
    std::vector<std::string> seen;

    seen.push_back(libPath.filename().string());
    queue.push_back(libPath);

    while (!queue.empty()) {
        fs::path cur = std::move(queue.back());
        queue.pop_back();

        for (const std::string& dep : parseElfDependencies(cur)) {
            if (std::find(seen.begin(), seen.end(), dep) != seen.end())
                continue;
            seen.push_back(dep);

            fs::path found;
            for (const fs::path& dir : searchDirs) {
                fs::path cand = dir / dep;
                if (fs::exists(cand)) {
                    found = cand;
                    break;
                }
            }
            if (found.empty()) continue;
            result.push_back(found);
            queue.push_back(std::move(found));
        }
    }
    return result;
}

// ---- Symbol lookup ---------------------------------------------------------

uintptr_t findElfExportOffset(const fs::path& libPath,
                              const std::string& symName) {
    auto data = readFile(libPath);
    if (!isElf(data) || !is64(data)) return 0;

    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());

    const Elf64_Shdr* dynsymSec = findSection(data, ehdr, ".dynsym");
    const Elf64_Shdr* dynstrSec = findSection(data, ehdr, ".dynstr");
    if (!dynsymSec || !dynstrSec) return 0;

    auto* syms = reinterpret_cast<const Elf64_Sym*>(
        data.data() + dynsymSec->sh_offset);
    size_t symCount = dynsymSec->sh_size / sizeof(Elf64_Sym);
    const char* strTab = reinterpret_cast<const char*>(
        data.data() + dynstrSec->sh_offset);

    for (size_t i = 0; i < symCount; ++i) {
        if (symName == (strTab + syms[i].st_name)) {
            return static_cast<uintptr_t>(syms[i].st_value);
        }
    }
    return 0;
}

// ---- SONAME -----------------------------------------------------------------

std::string readElfSoname(const fs::path& libPath) {
    auto data = readFile(libPath);
    if (!isElf(data) || !is64(data)) return {};

    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());

    // Find .dynamic section.
    const Elf64_Shdr* dynSec = findSection(data, ehdr, ".dynamic");
    const Elf64_Shdr* dynstrSec = findSection(data, ehdr, ".dynstr");
    if (!dynSec || !dynstrSec) return {};

    auto* dyn = reinterpret_cast<const Elf64_Dyn*>(
        data.data() + dynSec->sh_offset);
    size_t dynCount = dynSec->sh_size / sizeof(Elf64_Dyn);
    const char* strTab = reinterpret_cast<const char*>(
        data.data() + dynstrSec->sh_offset);

    for (size_t i = 0; i < dynCount; ++i) {
        if (dyn[i].d_tag == DT_SONAME) {
            return std::string(strTab + dyn[i].d_un.d_val);
        }
        if (dyn[i].d_tag == DT_NULL) break;
    }
    return {};
}

// ---- Architecture ---------------------------------------------------------

std::pair<std::string, int> elfArchitecture(const fs::path& libPath) {
    auto data = readFile(libPath);
    if (!isElf(data)) return {"unknown", 0};

    bool is64bit = (data[EI_CLASS] == ELFCLASS64);
    int bits = is64bit ? 64 : 32;

    if (is64bit) {
        auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data.data());
        // Only check for LE machines on Linux.
        switch (ehdr->e_machine) {
        case EM_X86_64: return {"x86_64", bits};
        case EM_AARCH64: return {"aarch64", bits};
        case EM_386: return {"i386", bits};
        default: return {"unknown", bits};
        }
    } else {
        auto* ehdr32 = reinterpret_cast<const Elf32_Ehdr*>(data.data());
        switch (ehdr32->e_machine) {
        case EM_386: return {"i386", bits};
        case EM_ARM: return {"arm", bits};
        default: return {"unknown", bits};
        }
    }
}

#endif // __linux__
