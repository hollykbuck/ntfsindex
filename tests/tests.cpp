#include <catch2/catch_test_macros.hpp>
#include "../src/ntfs_parser.h"
#include "../src/ntfs_indexer.h"

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

TEST_CASE("Resolve all paths test", "[ntfs]") {
    NtfsIndexer indexer;
    
    std::unordered_map<uint64_t, FileEntry> mock_files;
    
    // Root directory (record 5)
    mock_files[5] = FileEntry{5, 5, "root", true, 0, ""};
    
    // Dir A (record 100) -> child of root
    mock_files[100] = FileEntry{100, 5, "DirA", true, 0, ""};
    
    // File B (record 101) -> child of Dir A
    mock_files[101] = FileEntry{101, 100, "FileB.txt", false, 1234, ""};
    
    // Dir C (record 102) -> child of Dir A
    mock_files[102] = FileEntry{102, 100, "DirC", true, 0, ""};
    
    // File D (record 103) -> child of Dir C
    mock_files[103] = FileEntry{103, 102, "FileD.bin", false, 5678, ""};
    
    // Orphan File E (record 200) -> parent 999 (missing)
    mock_files[200] = FileEntry{200, 999, "OrphanE.txt", false, 999, ""};
    
    // Orphan Dir F (record 201) -> child of Orphan File E
    mock_files[201] = FileEntry{201, 200, "OrphanF", true, 0, ""};
    
    // Cycle Dir X (record 300) -> parent Y
    mock_files[300] = FileEntry{300, 301, "DirX", true, 0, ""};
    
    // Cycle Dir Y (record 301) -> parent X
    mock_files[301] = FileEntry{301, 300, "DirY", true, 0, ""};
    
    indexer.test_set_files(mock_files);
    indexer.test_resolve_all_paths();
    
    const auto& resolved = indexer.get_files();
    
    REQUIRE(resolved.at(5).full_path == "/");
    REQUIRE(resolved.at(100).full_path == "/DirA");
    REQUIRE(resolved.at(101).full_path == "/DirA/FileB.txt");
    REQUIRE(resolved.at(102).full_path == "/DirA/DirC");
    REQUIRE(resolved.at(103).full_path == "/DirA/DirC/FileD.bin");
    
    REQUIRE(resolved.at(200).full_path == "/[orphan]/OrphanE.txt");
    REQUIRE(resolved.at(201).full_path == "/[orphan]/OrphanE.txt/OrphanF");
    
    // Check cycle protection doesn't crash or loop infinitely
    REQUIRE_NOTHROW(resolved.at(300).full_path);
    REQUIRE_NOTHROW(resolved.at(301).full_path);
}
