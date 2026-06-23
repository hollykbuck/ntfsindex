#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <functional>
#include "ntfs_parser.h"

class NtfsIndexer {
public:
    NtfsIndexer();
    ~NtfsIndexer();

    // Perform the initial full MFT scan and build the index
    bool build_initial_index(NtfsParser& parser, std::function<void(uint64_t processed, uint64_t total)> progress_cb = nullptr);

    // Perform an incremental update based on USN Change Journal
    bool update_index_incremental(NtfsParser& parser);

    // Save and load cached index to/from disk
    bool save_to_cache(const std::string& cache_path) const;
    bool load_from_cache(const std::string& cache_path);

    // Get the in-memory files map
    const std::unordered_map<uint64_t, FileEntry>& get_files() const { return files_; }

    // Get the last parsed USN
    uint64_t get_last_usn() const { return last_usn_; }

    // Print summary scanning statistics
    void print_stats(const std::string& dev_path) const;

    // Reconstruct absolute path on-the-fly by traversing parent references
    std::string get_absolute_path(uint64_t id) const;

    // Direct tree manipulation helpers for testing
    void test_set_files(const std::unordered_map<uint64_t, FileEntry>& files) { files_ = files; }
    void test_set_last_usn(uint64_t usn) { last_usn_ = usn; }

private:
    std::unordered_map<uint64_t, FileEntry> files_;
    uint64_t last_usn_ = 0;
};
