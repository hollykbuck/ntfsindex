#include <catch2/catch_test_macros.hpp>
#include "../src/ntfs_parser.h"

TEST_CASE("UTF-16LE to UTF-8 conversion", "[utf16]") {
    // Test basic ASCII conversion
    uint16_t ascii[] = {'H', 'e', 'l', 'l', 'o'};
    REQUIRE(NtfsParser::utf16le_to_utf8(ascii, 5) == "Hello");

    // Test empty conversion
    REQUIRE(NtfsParser::utf16le_to_utf8(nullptr, 0) == "");
}

TEST_CASE("Unpack runs test", "[ntfs]") {
    // Run 1: length 8, offset 100
    // Header: 0x11 (1 byte length, 1 byte offset)
    // Length: 8 (0x08)
    // Offset: 100 (0x64)
    // Total run bytes: 0x11, 0x08, 0x64
    // Terminating byte: 0x00
    uint8_t run_data[] = { 0x11, 0x08, 0x64, 0x00 };
    std::vector<DataRun> runs;
    bool success = NtfsParser::unpack_runs(run_data, sizeof(run_data), runs);
    
    REQUIRE(success == true);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].vcn == 0);
    REQUIRE(runs[0].length == 8);
    REQUIRE(runs[0].lcn == 100);
}
