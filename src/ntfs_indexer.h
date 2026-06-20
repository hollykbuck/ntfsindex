#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include "ntfs_parser.h"

class NtfsIndexer {
public:
    NtfsIndexer();
    ~NtfsIndexer();

    // Perform the initial full MFT scan and build the index
    bool build_initial_index(NtfsParser& parser);

    // Perform an incremental update based on USN Change Journal
    bool update_index_incremental(NtfsParser& parser);

    // Get the in-memory files map
    const std::unordered_map<uint64_t, FileEntry>& get_files() const { return files_; }

    // Get the last parsed USN
    uint64_t get_last_usn() const { return last_usn_; }

    // Print summary scanning statistics
    void print_stats(const std::string& dev_path) const;

    // Direct tree manipulation helpers for testing
    void test_set_files(const std::unordered_map<uint64_t, FileEntry>& files) { files_ = files; }
    void test_resolve_all_paths() { resolve_all_paths(); }

private:
    void resolve_all_paths();

    std::unordered_map<uint64_t, FileEntry> files_;
    uint64_t last_usn_ = 0;
};
