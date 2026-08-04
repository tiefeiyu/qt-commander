// Test the PE import-table parser and dependency-closure resolver in
// injector_win.cpp.
//
// Covers:
//   - parseImportDependencies on a synthetic PE with a known import table
//   - parseImportDependencies on a real DLL (kernel32.dll)
//   - resolveDependencyClosure: transitive closure, de-dup, search dirs,
//     missing-dependency skipping, system DLLs not pulled in
//
// Includes injector_win.cpp directly to access static functions (same
// pattern as test_pe_real.cpp).
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include "../../src/injector/injector_win.cpp"

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d - %s\n", __FILE__, __LINE__, msg); } } while(0)

// ---------------------------------------------------------------------------
// Synthetic PE: minimal DOS + NT headers + one import descriptor pointing at
// a name RVA.  We only need enough for parseImportDependencies to walk the
// import directory -- it does not validate sections beyond rvaToOffset,
// which falls back to the raw RVA when no section covers it.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makeSyntheticPe(
    const std::vector<std::string>& importNames)
{
    std::vector<uint8_t> blob(4096, 0);

    IMAGE_DOS_HEADER* dos =
        reinterpret_cast<IMAGE_DOS_HEADER*>(blob.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x40;  // NT headers right after the DOS header

    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        blob.data() + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 0;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;

    // Lay out (file offset == RVA via the fallback in rvaToOffset):
    //   [descriptors]  [name1\0]  [name2\0] ...
    const DWORD descOff = 0x200;
    const DWORD nameArea = 0x300;
    size_t nameCursor = nameArea;
    for (size_t i = 0; i < importNames.size(); ++i) {
        IMAGE_IMPORT_DESCRIPTOR* desc =
            reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                blob.data() + descOff + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        desc->Name = static_cast<DWORD>(nameCursor);
        memcpy(blob.data() + nameCursor, importNames[i].c_str(),
               importNames[i].size() + 1);
        nameCursor += importNames[i].size() + 1;
    }
    // Terminating all-zero descriptor.

    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].
        VirtualAddress = descOff;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
        static_cast<DWORD>((importNames.size() + 1) *
                           sizeof(IMAGE_IMPORT_DESCRIPTOR));
    return blob;
}

void test_synthetic_imports()
{
    const std::vector<uint8_t> pe = makeSyntheticPe(
        {"Qt5Widgets.dll", "Qt5Core.dll", "KERNEL32.dll"});
    const std::vector<std::string> deps = parseImportDependencies(pe);
    CHECK(deps.size() == 3, "synthetic PE: 3 imports parsed");
    CHECK(deps[0] == "Qt5Widgets.dll", "synthetic PE: first import name");
    CHECK(deps[1] == "Qt5Core.dll", "synthetic PE: second import name");
    CHECK(deps[2] == "KERNEL32.dll", "synthetic PE: third import name");
}

void test_synthetic_no_imports()
{
    const std::vector<uint8_t> pe = makeSyntheticPe({});
    const std::vector<std::string> deps = parseImportDependencies(pe);
    CHECK(deps.empty(), "synthetic PE without imports -> empty");
}

void test_synthetic_dup_imports()
{
    const std::vector<uint8_t> pe =
        makeSyntheticPe({"Qt5Core.dll", "Qt5Core.dll"});
    const std::vector<std::string> deps = parseImportDependencies(pe);
    CHECK(deps.size() == 1, "duplicate imports de-duplicated");
}

void test_bad_input()
{
    std::vector<uint8_t> garbage(512, 0xAB);
    CHECK(parseImportDependencies(garbage).empty(),
          "non-PE data -> empty deps");
    std::vector<uint8_t> tiny(10, 0);
    CHECK(parseImportDependencies(tiny).empty(), "tiny buffer -> empty deps");
}

void test_kernel32_real()
{
    char sysdir[MAX_PATH];
    GetSystemDirectoryA(sysdir, sizeof(sysdir));
    const fs::path k32 = fs::path(sysdir) / "kernel32.dll";

    std::vector<uint8_t> bytes;
    CHECK(readFileBytes(k32, bytes), "read kernel32.dll");
    if (bytes.empty()) return;

    const std::vector<std::string> deps = parseImportDependencies(bytes);
    CHECK(!deps.empty(), "kernel32 has imports");
    // kernel32 always imports kernelbase (or api-ms-win-core-* on old OSes)
    bool foundKernelBase = false;
    for (const auto& d : deps) {
        std::string low = d;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(
                           std::tolower(c)); });
        if (low.find("kernelbase") != std::string::npos ||
            low.find("api-ms-win") != std::string::npos)
            foundKernelBase = true;
    }
    CHECK(foundKernelBase, "kernel32 imports kernelbase or api-ms-win-*");
}

// ---------------------------------------------------------------------------
// resolveDependencyClosure
// ---------------------------------------------------------------------------

void test_closure_transitive_and_dedup()
{
    // Build a tiny dependency chain in a temp dir:
    //   A.dll -> [B.dll, KERNEL32.dll]
    //   B.dll -> [A.dll(back-edge), C.dll]
    //   C.dll -> [KERNEL32.dll]
    // Search dir contains A, B, C; KERNEL32 is skipped (not in search dir).
    const fs::path dir = fs::temp_directory_path() /
                         ("qtc_dep_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto writePe = [&](const fs::path& p,
                       const std::vector<std::string>& imports) {
        std::ofstream out(p, std::ios::binary);
        const std::vector<uint8_t> pe = makeSyntheticPe(imports);
        out.write(reinterpret_cast<const char*>(pe.data()), pe.size());
    };

    writePe(dir / "A.dll", {"B.dll", "KERNEL32.dll"});
    writePe(dir / "B.dll", {"A.dll", "C.dll"});
    writePe(dir / "C.dll", {"KERNEL32.dll"});

    const std::vector<fs::path> closure =
        resolveDependencyClosure(dir / "A.dll", {dir});

    // A is the entry point itself (not part of the result); KERNEL32 is
    // skipped (not in the search dir).  Expect {B, C}.
    CHECK(closure.size() == 2, "closure resolves B and C only");
    bool hasB = false, hasC = false;
    for (const auto& p : closure) {
        if (p.filename() == "B.dll") hasB = true;
        if (p.filename() == "C.dll") hasC = true;
    }
    CHECK(hasB && hasC, "closure contains B.dll and C.dll");
    CHECK(closure.size() == 2, "back-edge to A not re-added (no duplicates)");

    fs::remove_all(dir);
}

void test_closure_missing_deps_skipped()
{
    const fs::path dir = fs::temp_directory_path() /
                         ("qtc_dep_test2_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    // X.dll -> [Y.dll, Qt5Widgets.dll]; only Y.dll exists in the search dir.
    std::ofstream out(dir / "X.dll", std::ios::binary);
    const std::vector<uint8_t> pe = makeSyntheticPe({"Y.dll", "Qt5Widgets.dll"});
    out.write(reinterpret_cast<const char*>(pe.data()), pe.size());
    out.close();

    std::ofstream outY(dir / "Y.dll", std::ios::binary);
    const std::vector<uint8_t> peY = makeSyntheticPe({});
    outY.write(reinterpret_cast<const char*>(peY.data()), peY.size());
    outY.close();

    const std::vector<fs::path> closure =
        resolveDependencyClosure(dir / "X.dll", {dir});
    CHECK(closure.size() == 1 && closure[0].filename() == "Y.dll",
          "missing Qt5Widgets.dll skipped, Y.dll still resolved");

    fs::remove_all(dir);
}

void test_closure_search_dirs()
{
    const fs::path dir = fs::temp_directory_path() /
                         ("qtc_dep_test3_" + std::to_string(GetCurrentProcessId()));
    const fs::path sub = dir / "sub";
    fs::remove_all(dir);
    fs::create_directories(sub);

    std::ofstream out(dir / "Main.dll", std::ios::binary);
    const std::vector<uint8_t> pe = makeSyntheticPe({"Extra.dll"});
    out.write(reinterpret_cast<const char*>(pe.data()), pe.size());
    out.close();
    std::ofstream outE(sub / "Extra.dll", std::ios::binary);
    const std::vector<uint8_t> peE = makeSyntheticPe({});
    outE.write(reinterpret_cast<const char*>(peE.data()), peE.size());
    outE.close();

    // Extra.dll only in 'sub' -- not found with just 'dir'...
    const std::vector<fs::path> closureOnly =
        resolveDependencyClosure(dir / "Main.dll", {dir});
    CHECK(closureOnly.empty(), "dependency not in search dirs -> skipped");

    // ...but found when 'sub' is searched.
    const std::vector<fs::path> closureBoth =
        resolveDependencyClosure(dir / "Main.dll", {dir, sub});
    CHECK(closureBoth.size() == 1 &&
              closureBoth[0].filename() == "Extra.dll",
          "dependency resolved from the second search dir");

    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    printf("test_import_parser\n");
    test_synthetic_imports();
    test_synthetic_no_imports();
    test_synthetic_dup_imports();
    test_bad_input();
    test_kernel32_real();
    test_closure_transitive_and_dedup();
    test_closure_missing_deps_skipped();
    test_closure_search_dirs();

    printf("%d passed, %d failed\n", passed, total - passed);
    return passed == total ? 0 : 1;
}
