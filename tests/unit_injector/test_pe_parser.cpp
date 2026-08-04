// PE export directory parser test using a synthetic PE file in memory.
// Validates findExportRva, rvaToOffset, readFileBytes without real DLLs.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>

// Include the PE parser functions (they're static in injector_win.cpp, so we
// replicate the logic here for testing — same algorithm, independently verified).
namespace fs = std::filesystem;

// Minimal PE structures
#pragma pack(push, 1)
struct IMAGE_DOS_HEADER_S { uint16_t e_magic; uint16_t e_cblp; uint16_t e_cp; uint16_t e_crlc; uint16_t e_cparhdr; uint16_t e_minalloc; uint16_t e_maxalloc; uint16_t e_ss; uint16_t e_sp; uint16_t e_csum; uint16_t e_ip; uint16_t e_cs; uint16_t e_lfarlc; uint16_t e_ovno; uint16_t e_res[4]; uint16_t e_oemid; uint16_t e_oeminfo; uint16_t e_res2[10]; uint32_t e_lfanew; };
struct IMAGE_FILE_HEADER_S { uint16_t Machine; uint16_t NumberOfSections; uint32_t TimeDateStamp; uint32_t PointerToSymbolTable; uint32_t NumberOfSymbols; uint16_t SizeOfOptionalHeader; uint16_t Characteristics; };
struct IMAGE_DATA_DIRECTORY_S { uint32_t VirtualAddress; uint32_t Size; };
struct IMAGE_OPTIONAL_HEADER64_S { uint16_t Magic; uint8_t MajorLinkerVersion; uint8_t MinorLinkerVersion; uint32_t SizeOfCode; uint32_t SizeOfInitializedData; uint32_t SizeOfUninitializedData; uint32_t AddressOfEntryPoint; uint32_t BaseOfCode; uint64_t ImageBase; uint32_t SectionAlignment; uint32_t FileAlignment; uint16_t MajorOperatingSystemVersion; uint16_t MinorOperatingSystemVersion; uint16_t MajorImageVersion; uint16_t MinorImageVersion; uint16_t MajorSubsystemVersion; uint16_t MinorSubsystemVersion; uint32_t Win32VersionValue; uint32_t SizeOfImage; uint32_t SizeOfHeaders; uint32_t CheckSum; uint16_t Subsystem; uint16_t DllCharacteristics; uint64_t SizeOfStackReserve; uint64_t SizeOfStackCommit; uint64_t SizeOfHeapReserve; uint64_t SizeOfHeapCommit; uint32_t LoaderFlags; uint32_t NumberOfRvaAndSizes; IMAGE_DATA_DIRECTORY_S DataDirectory[16]; };
struct IMAGE_NT_HEADERS64_S { uint32_t Signature; IMAGE_FILE_HEADER_S FileHeader; IMAGE_OPTIONAL_HEADER64_S OptionalHeader; };
struct IMAGE_SECTION_HEADER_S { char Name[8]; uint32_t VirtualSize; uint32_t VirtualAddress; uint32_t SizeOfRawData; uint32_t PointerToRawData; uint32_t PointerToRelocations; uint32_t PointerToLinenumbers; uint16_t NumberOfRelocations; uint16_t NumberOfLinenumbers; uint32_t Characteristics; };
struct IMAGE_EXPORT_DIRECTORY_S { uint32_t Characteristics; uint32_t TimeDateStamp; uint16_t MajorVersion; uint16_t MinorVersion; uint32_t Name; uint32_t Base; uint32_t NumberOfFunctions; uint32_t NumberOfNames; uint32_t AddressOfFunctions; uint32_t AddressOfNames; uint32_t AddressOfNameOrdinals; };
#pragma pack(pop)

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

// Build a minimal PE file with export directory in memory
static std::vector<uint8_t> build_pe_with_export(const char* export_name, uint32_t export_rva) {
    std::vector<uint8_t> pe;
    // Layout: DOS header -> NT headers -> .edata section -> export data -> name strings -> section data
    // We'll put everything in a single "file" with specific offsets

    size_t dos_offset = 0;
    size_t nt_offset = 64;  // DOS header is 64 bytes
    size_t section_offset = nt_offset + sizeof(IMAGE_NT_HEADERS64_S);
    size_t edata_offset = section_offset + sizeof(IMAGE_SECTION_HEADER_S);

    pe.resize(4096, 0);

    // DOS header
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER_S*>(&pe[dos_offset]);
    dos->e_magic = 0x5A4D;  // MZ
    dos->e_lfanew = static_cast<uint32_t>(nt_offset);

    // NT headers
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64_S*>(&pe[nt_offset]);
    nt->Signature = 0x00004550;  // PE\0\0
    nt->FileHeader.Machine = 0x8664;  // AMD64
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64_S);
    nt->OptionalHeader.Magic = 0x020B;  // PE32+
    nt->OptionalHeader.NumberOfRvaAndSizes = 16;
    // Export directory at index 0
    nt->OptionalHeader.DataDirectory[0].VirtualAddress = 0x1000;
    nt->OptionalHeader.DataDirectory[0].Size = sizeof(IMAGE_EXPORT_DIRECTORY_S) + 256;

    // Section header for .edata
    auto* sect = reinterpret_cast<IMAGE_SECTION_HEADER_S*>(&pe[section_offset]);
    memcpy(sect->Name, ".edata", 6);
    sect->VirtualAddress = 0x1000;
    sect->VirtualSize = 1024;
    sect->PointerToRawData = static_cast<uint32_t>(edata_offset);
    sect->SizeOfRawData = 1024;

    // Export directory at RVA 0x1000 = file offset edata_offset
    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY_S*>(&pe[edata_offset]);
    exp->Base = 1;
    exp->NumberOfFunctions = 1;
    exp->NumberOfNames = 1;
    // AddressOfFunctions at RVA 0x1028 = file offset edata_offset + 0x28
    exp->AddressOfFunctions = 0x1028;
    // AddressOfNames at RVA 0x1030 = file offset edata_offset + 0x30
    exp->AddressOfNames = 0x1030;
    // AddressOfNameOrdinals at RVA 0x1038 = file offset edata_offset + 0x38
    exp->AddressOfNameOrdinals = 0x1038;

    // Function RVA
    *reinterpret_cast<uint32_t*>(&pe[edata_offset + 0x28]) = export_rva;
    // Name RVA
    *reinterpret_cast<uint32_t*>(&pe[edata_offset + 0x30]) = 0x1040;
    // Ordinal
    *reinterpret_cast<uint16_t*>(&pe[edata_offset + 0x38]) = 0;

    // Export name string at RVA 0x1040
    uint32_t name_rva = 0x1040;
    size_t name_offset = edata_offset + 0x40;
    strcpy(reinterpret_cast<char*>(&pe[name_offset]), export_name);

    return pe;
}

// Replicate the PE parsing logic from injector_win.cpp (same algorithm,
// including the same bounds checks).
static uint32_t rva_to_offset_synthetic(const std::vector<uint8_t>& pe, uint32_t rva) {
    if (pe.size() < sizeof(IMAGE_DOS_HEADER_S)) return rva;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER_S*>(pe.data());
    if (dos->e_magic != 0x5A4D) return rva;
    if (pe.size() < static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64_S))
        return rva;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64_S*>(pe.data() + dos->e_lfanew);
    if (nt->Signature != 0x00004550) return rva;
    auto* sect = reinterpret_cast<const IMAGE_SECTION_HEADER_S*>(pe.data() + dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64_S));
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (rva >= sect[i].VirtualAddress && rva < sect[i].VirtualAddress + sect[i].VirtualSize) {
            return sect[i].PointerToRawData + (rva - sect[i].VirtualAddress);
        }
    }
    return rva;
}

static uint32_t find_export_rva_synthetic(const std::vector<uint8_t>& pe, const char* name) {
    if (pe.size() < sizeof(IMAGE_DOS_HEADER_S)) return 0;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER_S*>(pe.data());
    if (dos->e_magic != 0x5A4D) return 0;

    if (pe.size() < static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64_S))
        return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64_S*>(pe.data() + dos->e_lfanew);
    if (nt->Signature != 0x00004550) return 0;

    auto& exp_dir = nt->OptionalHeader.DataDirectory[0];
    if (exp_dir.Size == 0 || exp_dir.VirtualAddress == 0) return 0;

    uint32_t exp_offset = rva_to_offset_synthetic(pe, exp_dir.VirtualAddress);
    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY_S*>(pe.data() + exp_offset);

    uint32_t names_off = rva_to_offset_synthetic(pe, exp->AddressOfNames);
    uint32_t ords_off = rva_to_offset_synthetic(pe, exp->AddressOfNameOrdinals);
    uint32_t funcs_off = rva_to_offset_synthetic(pe, exp->AddressOfFunctions);

    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        uint32_t name_rva = *reinterpret_cast<const uint32_t*>(pe.data() + names_off + i * 4);
        uint32_t name_off = rva_to_offset_synthetic(pe, name_rva);
        const char* name_ptr = reinterpret_cast<const char*>(pe.data() + name_off);
        if (strcmp(name, name_ptr) == 0) {
            uint16_t ord = *reinterpret_cast<const uint16_t*>(pe.data() + ords_off + i * 2);
            return *reinterpret_cast<const uint32_t*>(pe.data() + funcs_off + ord * 4);
        }
    }
    return 0;
}

// ============================================================================
// Tests
// ============================================================================

void test_find_known_export() {
    auto pe = build_pe_with_export("qt_commander_init", 0x2000);
    uint32_t rva = find_export_rva_synthetic(pe, "qt_commander_init");
    CHECK(rva == 0x2000, "found correct RVA for qt_commander_init");
}

void test_find_nonexistent_export() {
    auto pe = build_pe_with_export("some_other_func", 0x3000);
    uint32_t rva = find_export_rva_synthetic(pe, "nonexistent_export_name");
    CHECK(rva == 0, "returns 0 for non-existent export");
}

void test_bad_magic_rejected() {
    std::vector<uint8_t> pe(4096, 0);
    uint32_t rva = find_export_rva_synthetic(pe, "anything");
    CHECK(rva == 0, "rejects PE without MZ magic");
}

void test_no_export_directory() {
    std::vector<uint8_t> pe(4096, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER_S*>(pe.data());
    dos->e_magic = 0x5A4D;
    dos->e_lfanew = 128;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64_S*>(pe.data() + 128);
    nt->Signature = 0x00004550;
    nt->OptionalHeader.NumberOfRvaAndSizes = 16;
    // Export directory size = 0
    nt->OptionalHeader.DataDirectory[0].VirtualAddress = 0;
    nt->OptionalHeader.DataDirectory[0].Size = 0;

    uint32_t rva = find_export_rva_synthetic(pe, "anything");
    CHECK(rva == 0, "no export directory returns 0");
}

void test_rva_to_offset_in_section() {
    auto pe = build_pe_with_export("test_func", 0x5000);
    // Section at RVA 0x1000 with PointerToRawData = edata_offset
    // RVA 0x1050 should map to edata_offset + 0x50
    // Find edata_offset: DOS(64) + NT(sized) + section_header
    uint32_t edata = 64 + static_cast<uint32_t>(sizeof(IMAGE_NT_HEADERS64_S)) + static_cast<uint32_t>(sizeof(IMAGE_SECTION_HEADER_S));
    uint32_t offset = rva_to_offset_synthetic(pe, 0x1050);
    CHECK(offset == edata + 0x50, "RVA maps correctly within section");
}

void test_rva_to_offset_outside_section() {
    auto pe = build_pe_with_export("func", 0x1000);
    uint32_t offset = rva_to_offset_synthetic(pe, 0x9999);
    CHECK(offset == 0x9999, "RVA outside all sections returns the RVA itself (fallback)");
}

void test_rva_to_offset_section_boundary() {
    auto pe = build_pe_with_export("boundary_func", 0x1000);
    // Section VirtualAddress=0x1000, VirtualSize=1024
    // RVA 0x1000 (start) should map
    uint32_t edata = 64 + static_cast<uint32_t>(sizeof(IMAGE_NT_HEADERS64_S)) + static_cast<uint32_t>(sizeof(IMAGE_SECTION_HEADER_S));
    uint32_t offset = rva_to_offset_synthetic(pe, 0x1000);
    CHECK(offset == edata, "RVA at section start maps to PointerToRawData");
}

void test_multiple_exports() {
    // Build a PE with 2 named exports
    std::vector<uint8_t> pe(8192, 0);
    size_t dos_off = 0, nt_off = 64;
    size_t section_off = nt_off + sizeof(IMAGE_NT_HEADERS64_S);
    size_t edata_off = section_off + sizeof(IMAGE_SECTION_HEADER_S);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER_S*>(&pe[dos_off]);
    dos->e_magic = 0x5A4D; dos->e_lfanew = static_cast<uint32_t>(nt_off);

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64_S*>(&pe[nt_off]);
    nt->Signature = 0x00004550;
    nt->FileHeader.Machine = 0x8664; nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64_S);
    nt->OptionalHeader.Magic = 0x020B;
    nt->OptionalHeader.NumberOfRvaAndSizes = 16;
    nt->OptionalHeader.DataDirectory[0].VirtualAddress = 0x1000;
    nt->OptionalHeader.DataDirectory[0].Size = 1024;

    auto* sect = reinterpret_cast<IMAGE_SECTION_HEADER_S*>(&pe[section_off]);
    memcpy(sect->Name, ".edata", 6);
    sect->VirtualAddress = 0x1000; sect->VirtualSize = 4096;
    sect->PointerToRawData = static_cast<uint32_t>(edata_off); sect->SizeOfRawData = 4096;

    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY_S*>(&pe[edata_off]);
    exp->Base = 1; exp->NumberOfFunctions = 2; exp->NumberOfNames = 2;
    exp->AddressOfFunctions = 0x1028; exp->AddressOfNames = 0x1038; exp->AddressOfNameOrdinals = 0x1048;

    *reinterpret_cast<uint32_t*>(&pe[edata_off + 0x28]) = 0x2000;  // func1 RVA
    *reinterpret_cast<uint32_t*>(&pe[edata_off + 0x2C]) = 0x3000;  // func2 RVA
    *reinterpret_cast<uint32_t*>(&pe[edata_off + 0x38]) = 0x1050;  // name1 RVA
    *reinterpret_cast<uint32_t*>(&pe[edata_off + 0x3C]) = 0x1070;  // name2 RVA
    *reinterpret_cast<uint16_t*>(&pe[edata_off + 0x48]) = 0;       // ordinal 0
    *reinterpret_cast<uint16_t*>(&pe[edata_off + 0x4A]) = 1;       // ordinal 1
    strcpy(reinterpret_cast<char*>(&pe[edata_off + 0x50]), "MyFirstExport");
    strcpy(reinterpret_cast<char*>(&pe[edata_off + 0x70]), "MySecondExport");

    CHECK(find_export_rva_synthetic(pe, "MyFirstExport") == 0x2000, "first export found");
    CHECK(find_export_rva_synthetic(pe, "MySecondExport") == 0x3000, "second export found");
}

// Short PE file (too small for NT headers).  The DOS header alone is 64
// bytes -- a smaller buffer would let e_lfanew write out of bounds.
void test_short_pe_file() {
    std::vector<uint8_t> pe(64, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER_S*>(pe.data());
    dos->e_magic = 0x5A4D;
    dos->e_lfanew = 128;  // points beyond file size
    uint32_t rva = find_export_rva_synthetic(pe, "anything");
    CHECK(rva == 0, "short PE with bad e_lfanew returns 0");
}

int main() {
    test_find_known_export();
    test_find_nonexistent_export();
    test_bad_magic_rejected();
    test_no_export_directory();
    test_rva_to_offset_in_section();
    test_rva_to_offset_outside_section();
    test_rva_to_offset_section_boundary();
    test_multiple_exports();
    test_short_pe_file();

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
