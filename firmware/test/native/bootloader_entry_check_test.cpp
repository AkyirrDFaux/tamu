// Native test for the bootloader entry stub post-link checker (scripts/bootloader_entry_check.py).
// This test validates the checker's logic by simulating nm/objdump output for various scenarios.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <sstream>

#include "check.h"

namespace
{
    constexpr uint32_t ENTRY_ADDR = 0x7FC;
    constexpr uint32_t REGION_END = 0x800;

    // Simulates the checker's nm parsing: finds BootloaderEntryStub address.
    // Returns address or UINT32_MAX if not found.
    uint32_t FindStubAddress(const std::string& nm_output)
    {
        std::istringstream iss(nm_output);
        std::string line;
        while (std::getline(iss, line))
        {
            std::istringstream ls(line);
            std::string addr_str, type, name;
            if (ls >> addr_str >> type >> name)
            {
                if (name == "BootloaderEntryStub")
                {
                    try
                    {
                        return static_cast<uint32_t>(std::stoul(addr_str, nullptr, 16));
                    }
                    catch (...)
                    {
                        return UINT32_MAX;
                    }
                }
            }
        }
        return UINT32_MAX;
    }

    // Simulates the checker's objdump -h parsing for .bootentry section.
    // Returns {found, vma, size}
    struct SectionInfo { bool found; uint32_t vma; uint32_t size; };
    SectionInfo FindBootentrySection(const std::string& objdump_output)
    {
        std::istringstream iss(objdump_output);
        std::string line;
        while (std::getline(iss, line))
        {
            if (line.find(".bootentry") != std::string::npos)
            {
                std::istringstream ls(line);
                std::string idx, name, size_str, vma_str, lma_str, file_off, algn;
                // objdump -h format: "idx name size vma lma file_off algn"
                if (ls >> idx >> name >> size_str >> vma_str >> lma_str >> file_off >> algn)
                {
                    try
                    {
                        uint32_t size = static_cast<uint32_t>(std::stoul(size_str, nullptr, 16));
                        uint32_t vma = static_cast<uint32_t>(std::stoul(vma_str, nullptr, 16));
                        return {true, vma, size};
                    }
                    catch (...)
                    {
                        continue;
                    }
                }
            }
        }
        return {false, 0, 0};
    }

    // Simulates the checker's section overlap check.
    // Returns empty string if OK, error message if overlap found.
    std::string CheckSectionOverlap(const std::string& objdump_output)
    {
        std::istringstream iss(objdump_output);
        std::string line;
        while (std::getline(iss, line))
        {
            std::istringstream ls(line);
            std::string idx, name, size_str, vma_str;
            if (!(ls >> idx >> name >> size_str >> vma_str)) continue;
            if (!isdigit(idx[0])) continue; // skip header lines
            if (name.find(".debug") == 0 || name == ".bootentry") continue;

            try
            {
                uint32_t size = static_cast<uint32_t>(std::stoul(size_str, nullptr, 16));
                uint32_t vma = static_cast<uint32_t>(std::stoul(vma_str, nullptr, 16));
                if (size && vma < REGION_END && vma + size > ENTRY_ADDR)
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "section %s (0x%X+0x%X) overlaps the entry word at 0x%X",
                                  name.c_str(), vma, size, ENTRY_ADDR);
                    return buf;
                }
            }
            catch (...)
            {
                continue;
            }
        }
        return "";
    }

    // Full checker simulation: returns empty string if PASS, error message if FAIL.
    std::string RunChecker(const std::string& nm_output, const std::string& objdump_output)
    {
        // Check 1: BootloaderEntryStub symbol exists and at 0x7FC
        uint32_t stub_addr = FindStubAddress(nm_output);
        if (stub_addr == UINT32_MAX)
        {
            return "BootloaderEntryStub is not in ELF (dropped by --gc-sections? --undefined=BootloaderEntryStub missing from build_flags)";
        }
        if (stub_addr != ENTRY_ADDR)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "BootloaderEntryStub is at 0x%X, not 0x%X", stub_addr, ENTRY_ADDR);
            return buf;
        }

        // Check 2: .bootentry section exists at 0x7FC and within region
        SectionInfo sec = FindBootentrySection(objdump_output);
        if (!sec.found)
        {
            return ".bootentry section is missing from ELF";
        }
        if (sec.vma != ENTRY_ADDR)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), ".bootentry VMA is 0x%X, not 0x%X", sec.vma, ENTRY_ADDR);
            return buf;
        }
        if (sec.vma + sec.size > REGION_END)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), ".bootentry runs past the 2 KB region (0x%X+0x%X > 0x%X)",
                          sec.vma, sec.size, REGION_END);
            return buf;
        }

        // Check 3: No other section overlaps the entry word
        std::string overlap = CheckSectionOverlap(objdump_output);
        if (!overlap.empty())
        {
            return overlap;
        }

        return ""; // PASS
    }

    // Helper to run a test case
    void TestCase(const char* name, const std::string& nm, const std::string& objdump, bool expect_pass)
    {
        std::string result = RunChecker(nm, objdump);
        bool pass = result.empty();
        if (pass == expect_pass)
        {
            CHECK(true);
            std::printf("PASS: %s\n", name);
        }
        else
        {
            CHECK(false);
            std::printf("FAIL: %s - expected %s, got: %s\n", name, expect_pass ? "PASS" : "FAIL", result.c_str());
        }
    }
}

int main()
{
    // ============================================================
    // Scenario 1: Correct build - stub at 0x7FC, .bootentry at 0x7FC, size 4, no overlap
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
  3 .bss          00000020  00000810  00000810  0000024e  2**2
)";
        TestCase("Correct build", nm, objdump, true);
    }

    // ============================================================
    // Scenario 2: Missing BootloaderEntryStub symbol (dropped by --gc-sections)
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase("Missing BootloaderEntryStub symbol", nm, objdump, false);
    }

    // ============================================================
    // Scenario 3: BootloaderEntryStub at wrong address
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007f8 T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007f8  000007f8  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase("BootloaderEntryStub at wrong address (0x7F8)", nm, objdump, false);
    }

    // ============================================================
    // Scenario 4: .bootentry section missing
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .data         00000010  00000800  00000800  0000023e  2**2
  2 .bss          00000020  00000810  00000810  0000024e  2**2
)";
        TestCase("Missing .bootentry section", nm, objdump, false);
    }

    // ============================================================
    // Scenario 5: .bootentry at wrong VMA
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007f8  000007f8  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase(".bootentry at wrong VMA (0x7F8)", nm, objdump, false);
    }

    // ============================================================
    // Scenario 6: .bootentry runs past region end (0x800)
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000008  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase(".bootentry runs past region (size 8 at 0x7FC)", nm, objdump, false);
    }

    // ============================================================
    // Scenario 7: Another section overlaps the entry word at 0x7FC
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000007fd  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase(".text overlaps entry word (ends at 0x7FD)", nm, objdump, false);
    }

    // ============================================================
    // Scenario 8: Section overlaps entry word from below (ends after 0x7FC)
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         00000800  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase(".text overlaps entry word (ends at 0x800)", nm, objdump, false);
    }

    // ============================================================
    // Scenario 9: .debug section should be ignored in overlap check
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007fc  000007fc  0000023a  2**1
  2 .debug_info   00001000  00000700  00000700  0000023e  2**0
  3 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase(".debug section overlapping entry word is ignored", nm, objdump, true);
    }

    // ============================================================
    // Scenario 10: Stub present but section size 0 (edge case)
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc T BootloaderEntryStub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000000  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase(".bootentry size 0 at correct VMA", nm, objdump, true);
    }

    // ============================================================
    // Scenario 11: Symbol name variant - lowercase/uppercase not matched
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000001c6 T BootloaderEntry
00000000000007fc t bootloaderentrystub
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase("Symbol name case mismatch (lowercase)", nm, objdump, false);
    }

    // ============================================================
    // Scenario 12: Multiple symbols, BootloaderEntryStub not the last
    // ============================================================
    {
        std::string nm = R"(
0000000000000000 T _start
00000000000007fc T BootloaderEntryStub
00000000000001c6 T BootloaderEntry
0000000000000800 T main
)";
        std::string objdump = R"(
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         000001c6  00000000  00000000  00000074  2**1
  1 .bootentry    00000004  000007fc  000007fc  0000023a  2**1
  2 .data         00000010  00000800  00000800  0000023e  2**2
)";
        TestCase("BootloaderEntryStub appears before BootloaderEntry in nm", nm, objdump, true);
    }

    std::printf("\n== bootloader entry check tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}