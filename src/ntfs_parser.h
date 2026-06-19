#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "ntfs_structs.h"

struct FileEntry {
    uint64_t id = 0;
    uint64_t parent_id = 0;
    std::string name;
    bool is_directory = false;
    uint64_t size = 0;
    std::string full_path;
};

struct DataRun {
    int64_t vcn = 0;  // Virtual Cluster Number
    int64_t lcn = 0;  // Logical Cluster Number (-1 if sparse)
    int64_t length = 0; // Length in clusters
};

class NtfsParser {
public:
    NtfsParser();
    ~NtfsParser();

    // Open partition or image file and read boot sector
    bool init(const std::string& dev_path);
    
    // Parse MFT and build file tree
    bool parse();

    // Get all parsed files
    const std::unordered_map<uint64_t, FileEntry>& get_files() const { return files_; }

    // Print parsed summary statistics
    void print_stats() const;

private:
    // Raw disk reading helpers
    bool read_disk(uint64_t offset, void* buffer, size_t size);
    bool read_from_runs(const std::vector<DataRun>& runs, uint64_t offset, void* buffer, size_t size);
    bool read_mft_record(uint64_t record_idx, std::vector<uint8_t>& buf);

    // Run-list parsing helper
    bool unpack_runs(const uint8_t* run_buf, size_t run_buf_size, std::vector<DataRun>& runs);
    
    // Fixup verification helper
    bool apply_fixups(uint8_t* buffer, size_t size, uint16_t fix_off, uint16_t fix_num);

    std::string dev_path_;
    int fd_ = -1;

    uint16_t sector_size_ = 512;
    uint32_t cluster_size_ = 4096;
    uint32_t record_size_ = 1024;
    uint64_t mft_start_offset_ = 0;
    uint64_t mft_total_size_ = 0;

    std::vector<DataRun> mft_runs_;
    std::unordered_map<uint64_t, FileEntry> files_;
};
