#pragma once
// ---------------------------------------------------------------------------
// elf_parser — parse ELF shared-object files (.so) for dependency analysis
// and symbol lookup.  Used on Linux instead of PE import/export parsing.
// ---------------------------------------------------------------------------
#ifdef __linux__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

// ---- dependency resolution -------------------------------------------------

/// Parse the DT_NEEDED entries of an ELF .so and return the SONAMEs
/// of its direct dependencies (e.g. "libQt5Core.so.5").
std::vector<std::string> parseElfDependencies(const fs::path& libPath);

/// Resolve the transitive dependency closure of an ELF shared library.
/// Dependencies found in `searchDirs` are included (recursively); those
/// not found are skipped (assumed already loaded or resolvable by ld.so).
/// The library itself is NOT included in the result.  Order is breadth-first.
std::vector<fs::path> resolveElfDependencyClosure(
    const fs::path& libPath,
    const std::vector<fs::path>& searchDirs);

// ---- symbol lookup ---------------------------------------------------------

/// Find the offset (st_value) of an exported symbol in an ELF .so.
/// For ET_DYN (PIE shared objects), st_value is relative to the load base
/// (i.e. it is already an offset, not a virtual address).
/// Returns 0 if the symbol is not found.
uintptr_t findElfExportOffset(const fs::path& libPath, const std::string& symName);

/// Read the first DT_NEEDED entry (SONAME) from an ELF .so.
std::string readElfSoname(const fs::path& libPath);

// ---- architecture detection ------------------------------------------------

/// Return a human-readable arch string ("x86_64", "aarch64", "i386", "unknown")
/// and bitness (32 or 64) from an ELF file.
std::pair<std::string, int> elfArchitecture(const fs::path& libPath);

#endif // __linux__
