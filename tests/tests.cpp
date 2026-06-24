#include <catch2/catch_test_macros.hpp>
#include "../src/ntfs_parser.h"
#include "absl/log/initialize.h"

namespace {
struct AbseilLogInitializer {
    AbseilLogInitializer() {
        absl::InitializeLog();
    }
};
static AbseilLogInitializer g_abseil_log_initializer;
} // namespace
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
    
    absl::node_hash_map<uint64_t, FileEntry> mock_files;
    
    // Root directory (record 5)
    mock_files[5] = FileEntry{5, 5, "root", true, 0};
    
    // Dir A (record 100) -> child of root
    mock_files[100] = FileEntry{100, 5, "DirA", true, 0};
    
    // File B (record 101) -> child of Dir A
    mock_files[101] = FileEntry{101, 100, "FileB.txt", false, 1234};
    
    // Dir C (record 102) -> child of Dir A
    mock_files[102] = FileEntry{102, 100, "DirC", true, 0};
    
    // File D (record 103) -> child of Dir C
    mock_files[103] = FileEntry{103, 102, "FileD.bin", false, 5678};
    
    // Orphan File E (record 200) -> parent 999 (missing)
    mock_files[200] = FileEntry{200, 999, "OrphanE.txt", false, 999};
    
    // Orphan Dir F (record 201) -> child of Orphan File E
    mock_files[201] = FileEntry{201, 200, "OrphanF", true, 0};
    
    // Cycle Dir X (record 300) -> parent Y
    mock_files[300] = FileEntry{300, 301, "DirX", true, 0};
    
    // Cycle Dir Y (record 301) -> parent X
    mock_files[301] = FileEntry{301, 300, "DirY", true, 0};
    
    indexer.test_set_files(mock_files);
    
    REQUIRE(indexer.get_absolute_path(5) == "/");
    REQUIRE(indexer.get_absolute_path(100) == "/DirA");
    REQUIRE(indexer.get_absolute_path(101) == "/DirA/FileB.txt");
    REQUIRE(indexer.get_absolute_path(102) == "/DirA/DirC");
    REQUIRE(indexer.get_absolute_path(103) == "/DirA/DirC/FileD.bin");
    
    REQUIRE(indexer.get_absolute_path(200) == "/[orphan]/OrphanE.txt");
    REQUIRE(indexer.get_absolute_path(201) == "/[orphan]/OrphanE.txt/OrphanF");
    
    // Check cycle protection doesn't crash or loop infinitely
    REQUIRE_NOTHROW(indexer.get_absolute_path(300));
    REQUIRE_NOTHROW(indexer.get_absolute_path(301));
}

#include <filesystem>

TEST_CASE("Cache save and load test", "[cache]") {
    NtfsIndexer indexer;
    
    absl::node_hash_map<uint64_t, FileEntry> mock_files;
    mock_files[5] = FileEntry{5, 5, "root", true, 0};
    mock_files[100] = FileEntry{100, 5, "DirA", true, 0};
    mock_files[101] = FileEntry{101, 100, "FileB.txt", false, 1234};
    
    indexer.test_set_files(mock_files);
    indexer.test_set_last_usn(0x123456789ABCDEF0ULL);
    
    const std::string cache_path = "test_cache.bin";
    
    // Clean up potentially pre-existing test file
    std::filesystem::remove(cache_path);
    
    SECTION("Save cache") {
        REQUIRE(indexer.save_to_cache(cache_path) == true);
        REQUIRE(std::filesystem::exists(cache_path) == true);
    }
    
    SECTION("Load cache") {
        REQUIRE(indexer.save_to_cache(cache_path) == true);
        
        NtfsIndexer loaded_indexer;
        REQUIRE(loaded_indexer.load_from_cache(cache_path) == true);
        
        REQUIRE(loaded_indexer.get_last_usn() == 0x123456789ABCDEF0ULL);
        const auto& loaded_files = loaded_indexer.get_files();
        REQUIRE(loaded_files.size() == 3);
        REQUIRE(loaded_files.at(5).name == "root");
        REQUIRE(loaded_files.at(100).name == "DirA");
        REQUIRE(loaded_files.at(101).name == "FileB.txt");
        REQUIRE(loaded_files.at(101).size == 1234);
        REQUIRE(loaded_indexer.get_absolute_path(101) == "/DirA/FileB.txt");
    }
    
    // Clean up
    std::filesystem::remove(cache_path);
}

#include <iostream>

TEST_CASE("Benchmark load cache", "[.benchmark]") {
    NtfsIndexer indexer;
    std::string success_path = "";
    auto start = std::chrono::high_resolution_clock::now();
    if (indexer.load_from_cache("data.bin")) {
        success_path = "data.bin";
    } else if (indexer.load_from_cache("../data.bin")) {
        success_path = "../data.bin";
    } else if (indexer.load_from_cache("../../data.bin")) {
        success_path = "../../data.bin";
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    if (!success_path.empty()) {
        std::cout << "\n[BENCHMARK] Loaded data.bin containing " << indexer.get_files().size()
                  << " files in " << diff.count() << " seconds.\n" << std::endl;
        
        std::cout << "[BENCHMARK] Re-saving cache to " << success_path << " in fast format..." << std::endl;
        auto save_start = std::chrono::high_resolution_clock::now();
        indexer.save_to_cache(success_path);
        auto save_end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> save_diff = save_end - save_start;
        std::cout << "[BENCHMARK] Saved in " << save_diff.count() << " seconds.\n" << std::endl;
    } else {
        std::cout << "\n[BENCHMARK] Failed to load data.bin\n" << std::endl;
    }
}

