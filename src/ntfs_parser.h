#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include "ntfs_structs.h"

struct FileEntry {
    uint64_t id = 0;
    uint64_t parent_id = 0;
    std::string name;
    bool is_directory = false;
    uint64_t size = 0;
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
    
    // Parse MFT metadata (runs, sizes)
    bool parse();

    // Get MFT record dimensions
    uint32_t record_size() const { return record_size_; }
    uint64_t mft_total_size() const { return mft_total_size_; }
    uint64_t mft_record_count() const { return mft_total_size_ / record_size_; }

    // Parse USN Change Journal ($Extend\$UsnJrnl) $J stream
    struct UsnJournalEntry {
        uint64_t usn = 0;
        uint64_t file_id = 0;
        uint64_t parent_id = 0;
        std::string filename;
        uint32_t reason = 0;
        uint64_t timestamp = 0; // Win32 FILETIME
    };

    bool parse_mft_record_to_entry(uint64_t idx, FileEntry& entry);
    bool parse_mft_record_to_entry(uint64_t idx, uint8_t* record_data, FileEntry& entry);
    bool read_mft_records_bulk(uint64_t start_idx, uint64_t count, uint8_t* dest_buf);
    bool parse_usn_journal(std::vector<UsnJournalEntry>& entries, uint64_t usn_mft_idx, uint64_t start_usn = 0, uint64_t* next_usn = nullptr);
    uint64_t query_current_usn(uint64_t usn_mft_idx);

    // Public static helpers for utility and testability
    static std::string utf16le_to_utf8(const uint16_t* utf16, size_t len);
    static bool unpack_runs(const uint8_t* run_buf, size_t run_buf_size, std::vector<DataRun>& runs);

private:
    // Raw disk reading helpers
    bool read_disk(uint64_t offset, void* buffer, size_t size);
    bool read_from_runs(const std::vector<DataRun>& runs, uint64_t offset, void* buffer, size_t size);
    bool read_mft_record(uint64_t record_idx, std::vector<uint8_t>& buf);
    
    // Fixup verification helper
    bool apply_fixups(uint8_t* buffer, size_t size, uint16_t fix_off, uint16_t fix_num);

    std::string dev_path_;
    std::ifstream stream_;
    bool is_raw_mft_ = false;

    uint16_t sector_size_ = 512;
    uint32_t cluster_size_ = 4096;
    uint32_t record_size_ = 1024;
    uint64_t mft_start_offset_ = 0;
    uint64_t mft_total_size_ = 0;

    std::vector<DataRun> mft_runs_;
};
