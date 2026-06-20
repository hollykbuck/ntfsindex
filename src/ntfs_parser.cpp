#include "ntfs_parser.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>

namespace {

// Get priority for name namespace: higher is better
int get_namespace_priority(uint8_t ns) {
    if (ns == 3) return 3; // Unicode & DOS
    if (ns == 1) return 2; // Unicode
    if (ns == 0) return 1; // POSIX
    if (ns == 2) return 0; // DOS (last choice)
    return -1;
}

// Formatting size in human readable format
std::string format_size(uint64_t bytes) {
    constexpr uint64_t KB = 1024;
    constexpr uint64_t MB = KB * 1024;
    constexpr uint64_t GB = MB * 1024;

    if (bytes >= GB) return fmt::format("{:.2f} GB", static_cast<double>(bytes) / GB);
    if (bytes >= MB) return fmt::format("{:.2f} MB", static_cast<double>(bytes) / MB);
    if (bytes >= KB) return fmt::format("{:.2f} KB", static_cast<double>(bytes) / KB);
    return fmt::format("{} Bytes", bytes);
}

} // namespace

// Helper to convert UTF-16LE to UTF-8
std::string NtfsParser::utf16le_to_utf8(const uint16_t* utf16, size_t len) {
    std::string utf8;
    utf8.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        uint32_t cp = utf16[i]; // system is little endian, so cast is correct
        if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate
            if (i + 1 < len) {
                uint32_t low = utf16[i + 1];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
        }
        if (cp < 0x80) {
            utf8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            utf8.push_back(static_cast<char>((cp >> 6) | 0xC0));
            utf8.push_back(static_cast<char>((cp & 0x3F) | 0x80));
        } else if (cp < 0x10000) {
            utf8.push_back(static_cast<char>((cp >> 12) | 0xE0));
            utf8.push_back(static_cast<char>(((cp >> 6) & 0x3F) | 0x80));
            utf8.push_back(static_cast<char>((cp & 0x3F) | 0x80));
        } else {
            utf8.push_back(static_cast<char>((cp >> 18) | 0xF0));
            utf8.push_back(static_cast<char>(((cp >> 12) & 0x3F) | 0x80));
            utf8.push_back(static_cast<char>(((cp >> 6) & 0x3F) | 0x80));
            utf8.push_back(static_cast<char>((cp & 0x3F) | 0x80));
        }
    }
    return utf8;
}

NtfsParser::NtfsParser() {}

NtfsParser::~NtfsParser() {
    if (stream_.is_open()) {
        stream_.close();
    }
}

bool NtfsParser::init(const std::string& dev_path) {
    dev_path_ = dev_path;
    stream_.open(std::filesystem::path(dev_path_), std::ios::binary);
    if (!stream_) {
        std::cerr << fmt::format("Error: Failed to open device/file '{}'\n", dev_path_);
        return false;
    }

    uint64_t file_size = 0;
    try {
        file_size = std::filesystem::file_size(std::filesystem::path(dev_path_));
    } catch (...) {
        // Fallback for some device partitions where file_size may throw
        stream_.seekg(0, std::ios::end);
        file_size = stream_.tellg();
        stream_.seekg(0, std::ios::beg);
    }

    uint32_t signature = 0;
    if (file_size >= 4) {
        if (!read_disk(0, &signature, 4)) {
            std::cerr << "Error: Failed to read file signature\n";
            return false;
        }
    }

    // Check if signature matches raw MFT record signature ('FILE')
    if (signature == SIGN_FILE) {
        is_raw_mft_ = true;
        sector_size_ = 512;
        cluster_size_ = 4096;
        mft_start_offset_ = 0;
        mft_total_size_ = file_size;

        // Read first record's total size (MFT record total size is at offset 28)
        uint32_t record_total = 0;
        if (file_size >= 32 && read_disk(28, &record_total, 4)) {
            if (record_total == 1024 || record_total == 2048 || record_total == 4096) {
                record_size_ = record_total;
            } else {
                record_size_ = 1024;
            }
        } else {
            record_size_ = 1024;
        }

        std::cout << fmt::format("[NTFS Info] Input recognized as a raw $MFT binary file.\n");
        std::cout << fmt::format("[NTFS Info] Total MFT size: {} bytes\n", mft_total_size_);
        std::cout << fmt::format("[NTFS Info] MFT record size: {} bytes\n", record_size_);
        return true;
    }

    is_raw_mft_ = false;

    // Read Boot Sector (VBR)
    std::vector<uint8_t> boot_buf(512);
    if (!read_disk(0, boot_buf.data(), 512)) {
        std::cerr << "Error: Failed to read Boot Sector\n";
        return false;
    }

    const NTFS_BOOT* boot = reinterpret_cast<const NTFS_BOOT*>(boot_buf.data());
    if (std::memcmp(boot->system_id, "NTFS    ", 8) != 0) {
        std::cerr << "Error: Device/File does not contain a valid NTFS boot sector or raw $MFT signature.\n";
        return false;
    }

    sector_size_ = (boot->bytes_per_sector[1] << 8) | boot->bytes_per_sector[0];
    cluster_size_ = sector_size_ * boot->sectors_per_cluster;

    if (boot->record_size > 0) {
        record_size_ = boot->record_size * cluster_size_;
    } else {
        record_size_ = 1 << (-boot->record_size);
    }

    mft_start_offset_ = boot->mft_clst * cluster_size_;

    std::cout << fmt::format("[NTFS Info] Sector size: {} bytes\n", sector_size_);
    std::cout << fmt::format("[NTFS Info] Cluster size: {} bytes\n", cluster_size_);
    std::cout << fmt::format("[NTFS Info] MFT record size: {} bytes\n", record_size_);
    std::cout << fmt::format("[NTFS Info] MFT start cluster: {} (Offset: 0x{:X})\n", boot->mft_clst, mft_start_offset_);

    return true;
}

bool NtfsParser::read_disk(uint64_t offset, void* buffer, size_t size) {
    if (size == 0) return true;
    if (!stream_.is_open()) return false;

    uint64_t align = sector_size_;
    if (align == 0) align = 512;

    uint64_t aligned_start = (offset / align) * align;
    uint64_t aligned_end = ((offset + size + align - 1) / align) * align;
    size_t aligned_size = static_cast<size_t>(aligned_end - aligned_start);

    if (offset == aligned_start && size == aligned_size) {
        stream_.clear();
        stream_.seekg(offset, std::ios::beg);
        if (!stream_) return false;
        stream_.read(reinterpret_cast<char*>(buffer), size);
        return !stream_.fail();
    } else {
        std::vector<uint8_t> temp_buf(aligned_size);
        stream_.clear();
        stream_.seekg(aligned_start, std::ios::beg);
        if (!stream_) return false;
        stream_.read(reinterpret_cast<char*>(temp_buf.data()), aligned_size);
        if (stream_.fail()) return false;

        uint64_t internal_offset = offset - aligned_start;
        std::memcpy(buffer, temp_buf.data() + internal_offset, size);
        return true;
    }
}

bool NtfsParser::unpack_runs(const uint8_t* run_buf, size_t run_buf_size, std::vector<DataRun>& runs) {
    const uint8_t* end = run_buf + run_buf_size;
    int64_t prev_lcn = 0;
    int64_t vcn = 0;

    while (run_buf < end) {
        uint8_t header = *run_buf++;
        if (header == 0) {
            break; // Terminating byte
        }

        uint8_t len_size = header & 0x0F;
        uint8_t offset_size = (header >> 4) & 0x0F;

        if (run_buf + len_size + offset_size > end) {
            return false; // Out of bounds
        }

        // Parse run length
        uint64_t len = 0;
        for (int i = 0; i < len_size; ++i) {
            len |= (static_cast<uint64_t>(run_buf[i]) << (i * 8));
        }
        run_buf += len_size;

        // Parse run offset (dlcn)
        int64_t dlcn = 0;
        if (offset_size > 0) {
            for (int i = 0; i < offset_size; ++i) {
                dlcn |= (static_cast<uint64_t>(run_buf[i]) << (i * 8));
            }
            run_buf += offset_size;

            // Sign extension
            if (run_buf[-1] & 0x80) {
                dlcn |= (~0ULL << (offset_size * 8));
            }
        }

        int64_t lcn;
        if (offset_size == 0) {
            lcn = -1; // Sparse
        } else {
            lcn = prev_lcn + dlcn;
            prev_lcn = lcn;
        }

        runs.push_back(DataRun{vcn, lcn, static_cast<int64_t>(len)});
        vcn += len;
    }
    return true;
}

bool NtfsParser::read_from_runs(const std::vector<DataRun>& runs, uint64_t offset, void* buffer, size_t size) {
    uint8_t* dest = reinterpret_cast<uint8_t*>(buffer);
    size_t bytes_left = size;
    uint64_t curr_offset = offset;

    while (bytes_left > 0) {
        uint64_t vcn = curr_offset / cluster_size_;
        uint64_t offset_in_cluster = curr_offset % cluster_size_;

        // Find run containing VCN
        const DataRun* target_run = nullptr;
        for (const auto& run : runs) {
            if (vcn >= static_cast<uint64_t>(run.vcn) && vcn < static_cast<uint64_t>(run.vcn + run.length)) {
                target_run = &run;
                break;
            }
        }

        if (!target_run) {
            // Cannot find VCN in runs, file offset out of range
            return false;
        }

        uint64_t run_offset_clusters = vcn - target_run->vcn;
        uint64_t clusters_remaining_in_run = target_run->length - run_offset_clusters;
        uint64_t max_bytes_in_run = clusters_remaining_in_run * cluster_size_ - offset_in_cluster;
        size_t chunk_size = std::min(static_cast<size_t>(max_bytes_in_run), bytes_left);

        if (target_run->lcn == -1) {
            // Sparse cluster, fill with zeros
            std::memset(dest, 0, chunk_size);
        } else {
            uint64_t physical_offset = (target_run->lcn + run_offset_clusters) * cluster_size_ + offset_in_cluster;
            if (!read_disk(physical_offset, dest, chunk_size)) {
                return false;
            }
        }

        dest += chunk_size;
        curr_offset += chunk_size;
        bytes_left -= chunk_size;
    }
    return true;
}

bool NtfsParser::apply_fixups(uint8_t* buffer, size_t size, uint16_t fix_off, uint16_t fix_num) {
    if (fix_num == 0) return true;
    if (static_cast<size_t>(fix_off) + static_cast<size_t>(fix_num) * 2 > size) return false;

    const uint16_t* usa = reinterpret_cast<const uint16_t*>(buffer + fix_off);
    uint16_t usn = usa[0];

    // Sector size is 512 bytes. Number of sectors to verify is fix_num - 1.
    size_t sectors = fix_num - 1;
    if (sectors * 512 > size) {
        return false;
    }

    uint16_t* ptr = reinterpret_cast<uint16_t*>(buffer + 512 - 2);
    for (size_t i = 0; i < sectors; ++i) {
        if (*ptr != usn) {
            // Fixup mismatch
            return false;
        }
        // Restore original bytes
        *ptr = usa[i + 1];
        ptr += 512 / 2; // Advance by 512 bytes (256 uint16_t words)
    }
    return true;
}

bool NtfsParser::read_mft_record(uint64_t record_idx, std::vector<uint8_t>& buf) {
    buf.resize(record_size_);
    
    if (mft_runs_.empty()) {
        // MFT was resident or read from initial boot start
        uint64_t physical_offset = mft_start_offset_ + record_idx * record_size_;
        return read_disk(physical_offset, buf.data(), record_size_);
    } else {
        uint64_t mft_offset = record_idx * record_size_;
        return read_from_runs(mft_runs_, mft_offset, buf.data(), record_size_);
    }
}

bool NtfsParser::parse() {
    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. Read MFT Record 0 ($MFT itself) if not in raw MFT mode
    if (is_raw_mft_) {
        // In raw $MFT mode, total size is already determined, and it is contiguous.
    } else {
        std::vector<uint8_t> mft_rec_buf;
        if (!read_mft_record(0, mft_rec_buf)) {
            std::cerr << "Error: Failed to read MFT Record 0\n";
            return false;
        }

        if (!apply_fixups(mft_rec_buf.data(), record_size_, 
                          reinterpret_cast<MFT_REC*>(mft_rec_buf.data())->rhdr.fix_off, 
                          reinterpret_cast<MFT_REC*>(mft_rec_buf.data())->rhdr.fix_num)) {
            std::cerr << "Error: MFT Record 0 fixup validation failed\n";
            return false;
        }

        MFT_REC* mft_rec = reinterpret_cast<MFT_REC*>(mft_rec_buf.data());
        if (mft_rec->rhdr.sign != SIGN_FILE) {
            std::cerr << "Error: MFT Record 0 does not have a valid 'FILE' signature\n";
            return false;
        }

        // Parse attributes in MFT Record 0 to extract $DATA run lists
        uint32_t offset = mft_rec->attr_off;
        bool found_mft_data = false;

        while (offset + 24 <= mft_rec->used) {
            const ATTRIB* attrib = reinterpret_cast<const ATTRIB*>(mft_rec_buf.data() + offset);
            if (attrib->type == ATTR_END) {
                break;
            }

            if (offset + attrib->size > mft_rec->used) {
                break;
            }

            if (attrib->type == ATTR_DATA) {
                if (attrib->non_res) {
                    // Parse run lists
                    const uint8_t* run_buf = reinterpret_cast<const uint8_t*>(attrib) + attrib->nres.run_off;
                    size_t run_buf_size = attrib->size - attrib->nres.run_off;
                    if (unpack_runs(run_buf, run_buf_size, mft_runs_)) {
                        mft_total_size_ = attrib->nres.data_size;
                        found_mft_data = true;
                    }
                } else {
                    // Resident MFT (very small filesystem)
                    mft_total_size_ = attrib->res.data_size;
                    found_mft_data = true;
                }
                break;
            }

            offset += attrib->size;
            if (attrib->size == 0) break;
        }

        if (!found_mft_data) {
            std::cerr << "Error: $DATA attribute not found in MFT Record 0\n";
            return false;
        }
    }

    uint64_t num_records = mft_total_size_ / record_size_;
    std::cout << fmt::format("[MFT Parse] MFT Total Size: {} ({})\n", mft_total_size_, format_size(mft_total_size_));
    std::cout << fmt::format("[MFT Parse] MFT Records Count: {}\n", num_records);

    // 2. Loop through all MFT records
    files_.reserve(num_records);
    for (uint64_t idx = 0; idx < num_records; ++idx) {
        FileEntry entry;
        if (parse_mft_record_to_entry(idx, entry)) {
            files_[idx] = entry;
        }
    }

    // 3. Resolve paths
    resolve_all_paths();

    // 4. Query and initialize last USN
    last_usn_ = query_current_usn();
    std::cout << fmt::format("[USN Init] Initialized last USN position to: 0x{:X}\n", last_usn_);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << fmt::format("[Scan Completed] Scanned {} files/dirs in {} ms\n", files_.size(), elapsed.count());

    return true;
}

bool NtfsParser::parse_mft_record_to_entry(uint64_t idx, FileEntry& entry) {
    std::vector<uint8_t> rec_buf;
    if (!read_mft_record(idx, rec_buf)) {
        return false;
    }

    MFT_REC* rec = reinterpret_cast<MFT_REC*>(rec_buf.data());

    // Perform Fixups
    if (!apply_fixups(rec_buf.data(), record_size_, rec->rhdr.fix_off, rec->rhdr.fix_num)) {
        return false;
    }

    // Validate record
    if (rec->rhdr.sign != SIGN_FILE) {
        return false;
    }

    // Only parse active base MFT records
    if (!(rec->flags & RECORD_FLAG_IN_USE)) {
        return false;
    }

    if (rec->parent_ref.get_record_id() != 0) {
        return false; // Skip non-base/extension records
    }

    // Gather details
    uint64_t parent_id = 0;
    std::string best_name;
    int best_ns_priority = -1;
    uint64_t file_size = 0;
    bool size_from_data = false;
    bool has_name = false;

    // Iterate over attributes in record
    uint32_t attr_offset = rec->attr_off;
    while (attr_offset + 24 <= rec->used) {
        const ATTRIB* attrib = reinterpret_cast<const ATTRIB*>(rec_buf.data() + attr_offset);
        if (attrib->type == ATTR_END) {
            break;
        }

        if (attr_offset + attrib->size > rec->used) {
            break; // Attribute size goes beyond used record size
        }

        if (attrib->type == ATTR_NAME) {
            // File Name Attribute
            if (!attrib->non_res) {
                const ATTR_FILE_NAME* fname = reinterpret_cast<const ATTR_FILE_NAME*>(
                    reinterpret_cast<const uint8_t*>(attrib) + attrib->res.data_off
                );
                
                // Verify size
                if (attrib->res.data_off + sizeof(ATTR_FILE_NAME) <= attrib->size) {
                    int priority = get_namespace_priority(fname->type);
                    if (priority > best_ns_priority) {
                        best_ns_priority = priority;
                        parent_id = fname->home.get_record_id();
                        best_name = utf16le_to_utf8(fname->name, fname->name_len);
                        has_name = true;
                        // Fallback file size
                        if (!size_from_data) {
                            file_size = fname->dup.data_size;
                        }
                    }
                }
            }
        } else if (attrib->type == ATTR_DATA) {
            // Data Attribute
            // Default data stream usually has name_len == 0
            if (attrib->name_len == 0) {
                if (attrib->non_res) {
                    file_size = attrib->nres.data_size;
                } else {
                    file_size = attrib->res.data_size;
                }
                size_from_data = true;
            }
        }

        attr_offset += attrib->size;
        if (attrib->size == 0) break;
    }

    if (has_name) {
        entry.id = idx;
        entry.parent_id = parent_id;
        entry.name = best_name;
        entry.is_directory = (rec->flags & RECORD_FLAG_DIR);
        entry.size = entry.is_directory ? 0 : file_size;
        return true;
    }

    return false;
}

void NtfsParser::resolve_all_paths() {
    // Root directory (record 5)
    files_[5].full_path = "/";
    files_[5].is_directory = true;

    for (auto& [id, entry] : files_) {
        if (id == 5) continue;

        std::vector<uint64_t> path_ids;
        uint64_t curr = id;
        std::unordered_map<uint64_t, bool> visited;

        while (curr != 5 && curr != 0) {
            if (visited[curr]) {
                break; // Cycle detected
            }
            visited[curr] = true;

            auto it = files_.find(curr);
            if (it == files_.end()) {
                break; // Missing parent record (orphan)
            }

            path_ids.push_back(curr);
            curr = it->second.parent_id;
        }

        std::string path;
        if (curr == 5) {
            // Path reached root
            for (auto r_it = path_ids.rbegin(); r_it != path_ids.rend(); ++r_it) {
                path += "/" + files_[*r_it].name;
            }
        } else {
            // Orphan path
            path = "/[orphan]";
            for (auto r_it = path_ids.rbegin(); r_it != path_ids.rend(); ++r_it) {
                path += "/" + files_[*r_it].name;
            }
        }
        entry.full_path = path;
    }
}

void NtfsParser::print_stats() const {
    size_t file_count = 0;
    size_t dir_count = 0;
    uint64_t total_bytes = 0;

    for (const auto& [id, entry] : files_) {
        if (entry.is_directory) {
            dir_count++;
        } else {
            file_count++;
            total_bytes += entry.size;
        }
    }

    std::cout << "\n================ NTFS Scan Statistics ================\n";
    std::cout << fmt::format("Device Path:         {}\n", dev_path_);
    std::cout << fmt::format("Total Directories:   {}\n", dir_count);
    std::cout << fmt::format("Total Files:         {}\n", file_count);
    std::cout << fmt::format("Total Logical Size:  {} ({})\n", total_bytes, format_size(total_bytes));
    std::cout << "======================================================\n\n";
}

bool NtfsParser::parse_usn_journal(std::vector<UsnJournalEntry>& entries, uint64_t start_usn, uint64_t* next_usn) {
    uint64_t usn_mft_idx = 0;
    bool found_usn_file = false;

    // 1. Locate $UsnJrnl in the pre-parsed files
    for (const auto& [id, entry] : files_) {
        // Parent MFT 11 is $Extend
        if (entry.parent_id == 11 && entry.name == "$UsnJrnl") {
            usn_mft_idx = id;
            found_usn_file = true;
            break;
        }
    }

    if (!found_usn_file) {
        return false;
    }

    // 2. Read $UsnJrnl MFT record
    std::vector<uint8_t> rec_buf;
    if (!read_mft_record(usn_mft_idx, rec_buf)) {
        std::cerr << "Error: Failed to read MFT record for $UsnJrnl.\n";
        return false;
    }
    
    MFT_REC* rec = reinterpret_cast<MFT_REC*>(rec_buf.data());
    if (!apply_fixups(rec_buf.data(), record_size_, rec->rhdr.fix_off, rec->rhdr.fix_num)) {
        std::cerr << "Error: MFT record fixup verification failed for $UsnJrnl.\n";
        return false;
    }

    // 3. Locate the $J Data Stream attribute or $ATTRIBUTE_LIST
    const ATTRIB* j_attrib = nullptr;
    const ATTRIB* al_attrib = nullptr;
    uint32_t attr_offset = rec->attr_off;
    while (attr_offset + 24 <= rec->used) {
        const ATTRIB* attrib = reinterpret_cast<const ATTRIB*>(rec_buf.data() + attr_offset);
        if (attrib->type == ATTR_END) break;
        if (attr_offset + attrib->size > rec->used) break;

        std::string name = "";
        if (attrib->name_len > 0) {
            const uint16_t* name_ptr = reinterpret_cast<const uint16_t*>(
                reinterpret_cast<const uint8_t*>(attrib) + attrib->name_off
            );
            name = utf16le_to_utf8(name_ptr, attrib->name_len);
        }

        if (attrib->type == ATTR_DATA && name == "$J") {
            j_attrib = attrib;
        } else if (attrib->type == ATTR_LIST) {
            al_attrib = attrib;
        }

        attr_offset += attrib->size;
        if (attrib->size == 0) break;
    }

    std::vector<DataRun> j_runs;
    uint64_t j_stream_size = 0;
    bool found_j_stream = false;

    if (j_attrib) {
        if (j_attrib->non_res) {
            const uint8_t* run_buf = reinterpret_cast<const uint8_t*>(j_attrib) + j_attrib->nres.run_off;
            size_t run_buf_size = j_attrib->size - j_attrib->nres.run_off;
            if (unpack_runs(run_buf, run_buf_size, j_runs)) {
                j_stream_size = j_attrib->nres.data_size;
                found_j_stream = true;
            }
        } else {
            std::cerr << "Error: $J stream attribute in base record is resident (unexpected for change journal).\n";
            return false;
        }
    } else if (al_attrib) {
        // Read the attribute list content
        std::vector<uint8_t> attr_list_buf;
        if (!al_attrib->non_res) {
            // Resident attribute list
            if (al_attrib->res.data_off + al_attrib->res.data_size <= al_attrib->size) {
                const uint8_t* data_ptr = reinterpret_cast<const uint8_t*>(al_attrib) + al_attrib->res.data_off;
                attr_list_buf.assign(data_ptr, data_ptr + al_attrib->res.data_size);
            }
        } else {
            // Non-resident attribute list
            std::vector<DataRun> al_runs;
            const uint8_t* run_buf = reinterpret_cast<const uint8_t*>(al_attrib) + al_attrib->nres.run_off;
            size_t run_buf_size = al_attrib->size - al_attrib->nres.run_off;
            if (unpack_runs(run_buf, run_buf_size, al_runs)) {
                attr_list_buf.resize(al_attrib->nres.data_size);
                if (!read_from_runs(al_runs, 0, attr_list_buf.data(), attr_list_buf.size())) {
                    std::cerr << "Error: Failed to read non-resident attribute list.\n";
                    return false;
                }
            }
        }

        if (!attr_list_buf.empty()) {
#pragma pack(push, 1)
            struct ATTR_LIST_ENTRY {
                uint32_t type;
                uint16_t record_length;
                uint8_t  name_length;
                uint8_t  name_offset;
                uint64_t lowest_vcn;
                uint64_t mft_reference;
                uint16_t attribute_id;
                uint16_t name[1];
            };
#pragma pack(pop)

            size_t al_offset = 0;
            while (al_offset + 0x1A <= attr_list_buf.size()) {
                const ATTR_LIST_ENTRY* entry = reinterpret_cast<const ATTR_LIST_ENTRY*>(attr_list_buf.data() + al_offset);
                if (entry->record_length == 0) break;
                if (al_offset + entry->record_length > attr_list_buf.size()) break;

                if (entry->type == ATTR_DATA) {
                    std::string entry_name = "";
                    if (entry->name_length > 0 && entry->name_offset + entry->name_length * 2 <= entry->record_length) {
                        const uint16_t* name_ptr = reinterpret_cast<const uint16_t*>(
                            reinterpret_cast<const uint8_t*>(entry) + entry->name_offset
                        );
                        entry_name = utf16le_to_utf8(name_ptr, entry->name_length);
                    }

                    if (entry_name == "$J") {
                        uint64_t ext_mft_idx = entry->mft_reference & 0x0000FFFFFFFFFFFFULL;
                        std::vector<uint8_t> ext_rec_buf;
                        if (read_mft_record(ext_mft_idx, ext_rec_buf)) {
                            MFT_REC* ext_rec = reinterpret_cast<MFT_REC*>(ext_rec_buf.data());
                            if (apply_fixups(ext_rec_buf.data(), record_size_, ext_rec->rhdr.fix_off, ext_rec->rhdr.fix_num)) {
                                uint32_t ext_attr_offset = ext_rec->attr_off;
                                while (ext_attr_offset + 24 <= ext_rec->used) {
                                    const ATTRIB* ext_attrib = reinterpret_cast<const ATTRIB*>(ext_rec_buf.data() + ext_attr_offset);
                                    if (ext_attrib->type == ATTR_END) break;
                                    if (ext_attr_offset + ext_attrib->size > ext_rec->used) break;

                                    if (ext_attrib->type == ATTR_DATA) {
                                        std::string ext_name = "";
                                        if (ext_attrib->name_len > 0) {
                                            const uint16_t* ext_name_ptr = reinterpret_cast<const uint16_t*>(
                                                reinterpret_cast<const uint8_t*>(ext_attrib) + ext_attrib->name_off
                                            );
                                            ext_name = utf16le_to_utf8(ext_name_ptr, ext_attrib->name_len);
                                        }

                                        if (ext_name == "$J" && ext_attrib->id == entry->attribute_id) {
                                            if (ext_attrib->non_res) {
                                                std::vector<DataRun> seg_runs;
                                                const uint8_t* ext_run_buf = reinterpret_cast<const uint8_t*>(ext_attrib) + ext_attrib->nres.run_off;
                                                size_t ext_run_buf_size = ext_attrib->size - ext_attrib->nres.run_off;
                                                if (unpack_runs(ext_run_buf, ext_run_buf_size, seg_runs)) {
                                                    uint64_t svcn = ext_attrib->nres.svcn;
                                                    for (auto& run : seg_runs) {
                                                        run.vcn += svcn;
                                                        j_runs.push_back(run);
                                                    }
                                                    j_stream_size = std::max(j_stream_size, ext_attrib->nres.data_size);
                                                    found_j_stream = true;
                                                }
                                            }
                                            break;
                                        }
                                    }
                                    ext_attr_offset += ext_attrib->size;
                                    if (ext_attrib->size == 0) break;
                                }
                            }
                        }
                    }
                }
                al_offset += entry->record_length;
            }

            if (found_j_stream) {
                // Sort runs by VCN
                std::sort(j_runs.begin(), j_runs.end(), [](const DataRun& a, const DataRun& b) {
                    return a.vcn < b.vcn;
                });
                std::cout << "[USN Debug] j_runs size: " << j_runs.size() << "\n";
                if (!j_runs.empty()) {
                    std::cout << fmt::format("  First Run: VCN {} to {}, LCN {}\n", j_runs.front().vcn, j_runs.front().vcn + j_runs.front().length, j_runs.front().lcn);
                    std::cout << fmt::format("  Last Run: VCN {} to {}, LCN {}\n", j_runs.back().vcn, j_runs.back().vcn + j_runs.back().length, j_runs.back().lcn);
                }
            }
        }
    }

    if (!found_j_stream) {
        std::cerr << "Error: $J stream attribute not found in $UsnJrnl MFT base or extension records.\n";
        return false;
    }

    // 5. Find the start of active (non-sparse) data
    uint64_t active_offset = 0;
    for (const auto& run : j_runs) {
        if (run.lcn != -1) {
            active_offset = run.vcn * cluster_size_;
            break;
        }
    }

    std::cout << fmt::format("[USN Parse] $J stream size: {} bytes\n", j_stream_size);
    std::cout << fmt::format("[USN Parse] First active record offset: 0x{:X}\n", active_offset);

    // 6. Read and parse USN records sequentially
    uint64_t curr_offset = active_offset;
    if (start_usn > 0) {
        if (start_usn < active_offset) {
            std::cout << fmt::format("[USN Parse] start_usn 0x{:X} is older than active offset 0x{:X}. Journal was truncated.\n", start_usn, active_offset);
            curr_offset = active_offset;
        } else if (start_usn > j_stream_size) {
            std::cout << fmt::format("[USN Parse] start_usn 0x{:X} is beyond stream size 0x{:X}.\n", start_usn, j_stream_size);
            curr_offset = j_stream_size;
        } else {
            curr_offset = start_usn;
        }
    }

    std::vector<uint8_t> block_buf(256 * 1024); // Read in 256KB chunks

    while (curr_offset < j_stream_size) {
        size_t bytes_to_read = std::min(block_buf.size(), static_cast<size_t>(j_stream_size - curr_offset));
        if (bytes_to_read < sizeof(USN_RECORD_V2)) break;

        if (!read_from_runs(j_runs, curr_offset, block_buf.data(), bytes_to_read)) {
            std::cerr << fmt::format("Error: Failed reading stream at offset 0x{:X}\n", curr_offset);
            break;
        }

        size_t block_offset = 0;
        while (block_offset + sizeof(USN_RECORD_V2) <= bytes_to_read) {
            const USN_RECORD_V2* record = reinterpret_cast<const USN_RECORD_V2*>(block_buf.data() + block_offset);

            // If we encounter a zero-length padding block (e.g. alignment fill or unwritten space)
            if (record->record_length == 0) {
                // If it is all zeroes, skip to the next 8-byte boundary or the next sector
                block_offset += 8;
                curr_offset += 8;
                continue;
            }

            // Boundary validation
            if (block_offset + record->record_length > bytes_to_read) {
                // The record spans across the chunk boundary, reload at curr_offset
                break;
            }

            // Self-validation: Verify the USN offset matches the stream offset
            if (record->major_version == 2 && record->usn == curr_offset) {
                UsnJournalEntry entry;
                entry.usn = record->usn;
                entry.file_id = record->file_reference_number & 0x0000FFFFFFFFFFFFULL; // Lower 48-bits
                entry.parent_id = record->parent_file_ref_num & 0x0000FFFFFFFFFFFFULL;
                entry.reason = record->reason;
                entry.timestamp = record->timestamp;

                // Extract filename
                if (static_cast<uint32_t>(record->name_offset) + record->name_length <= record->record_length) {
                    const uint16_t* name_ptr = reinterpret_cast<const uint16_t*>(
                        reinterpret_cast<const uint8_t*>(record) + record->name_offset
                    );
                    entry.filename = utf16le_to_utf8(name_ptr, record->name_length / 2);
                }

                entries.push_back(entry);
            }

            // Advance by record length, aligned to 8 bytes
            uint32_t aligned_len = (record->record_length + 7) & ~7;
            block_offset += aligned_len;
            curr_offset += aligned_len;
        }
    }

    if (next_usn) {
        *next_usn = curr_offset;
    }

    return true;
}

uint64_t NtfsParser::query_current_usn() {
    uint64_t usn_mft_idx = 0;
    bool found_usn_file = false;
    for (const auto& [id, entry] : files_) {
        if (entry.parent_id == 11 && entry.name == "$UsnJrnl") {
            usn_mft_idx = id;
            found_usn_file = true;
            break;
        }
    }
    if (!found_usn_file) return 0;

    std::vector<uint8_t> rec_buf;
    if (!read_mft_record(usn_mft_idx, rec_buf)) return 0;
    MFT_REC* rec = reinterpret_cast<MFT_REC*>(rec_buf.data());
    if (!apply_fixups(rec_buf.data(), record_size_, rec->rhdr.fix_off, rec->rhdr.fix_num)) return 0;

    const ATTRIB* j_attrib = nullptr;
    const ATTRIB* al_attrib = nullptr;
    uint32_t attr_offset = rec->attr_off;
    while (attr_offset + 24 <= rec->used) {
        const ATTRIB* attrib = reinterpret_cast<const ATTRIB*>(rec_buf.data() + attr_offset);
        if (attrib->type == ATTR_END) break;
        if (attr_offset + attrib->size > rec->used) break;

        std::string name = "";
        if (attrib->name_len > 0) {
            const uint16_t* name_ptr = reinterpret_cast<const uint16_t*>(
                reinterpret_cast<const uint8_t*>(attrib) + attrib->name_off
            );
            name = utf16le_to_utf8(name_ptr, attrib->name_len);
        }
        if (attrib->type == ATTR_DATA && name == "$J") {
            j_attrib = attrib;
        } else if (attrib->type == ATTR_LIST) {
            al_attrib = attrib;
        }
        attr_offset += attrib->size;
        if (attrib->size == 0) break;
    }

    if (j_attrib) {
        return j_attrib->nres.data_size;
    } else if (al_attrib) {
        std::vector<uint8_t> attr_list_buf;
        if (!al_attrib->non_res) {
            if (al_attrib->res.data_off + al_attrib->res.data_size <= al_attrib->size) {
                const uint8_t* data_ptr = reinterpret_cast<const uint8_t*>(al_attrib) + al_attrib->res.data_off;
                attr_list_buf.assign(data_ptr, data_ptr + al_attrib->res.data_size);
            }
        } else {
            std::vector<DataRun> al_runs;
            const uint8_t* run_buf = reinterpret_cast<const uint8_t*>(al_attrib) + al_attrib->nres.run_off;
            size_t run_buf_size = al_attrib->size - al_attrib->nres.run_off;
            if (unpack_runs(run_buf, run_buf_size, al_runs)) {
                attr_list_buf.resize(al_attrib->nres.data_size);
                read_from_runs(al_runs, 0, attr_list_buf.data(), attr_list_buf.size());
            }
        }

        if (!attr_list_buf.empty()) {
#pragma pack(push, 1)
            struct ATTR_LIST_ENTRY {
                uint32_t type;
                uint16_t record_length;
                uint8_t  name_length;
                uint8_t  name_offset;
                uint64_t lowest_vcn;
                uint64_t mft_reference;
                uint16_t attribute_id;
                uint16_t name[1];
            };
#pragma pack(pop)

            size_t al_offset = 0;
            while (al_offset + 0x1A <= attr_list_buf.size()) {
                const ATTR_LIST_ENTRY* entry = reinterpret_cast<const ATTR_LIST_ENTRY*>(attr_list_buf.data() + al_offset);
                if (entry->record_length == 0) break;
                if (al_offset + entry->record_length > attr_list_buf.size()) break;

                if (entry->type == ATTR_DATA) {
                    std::string entry_name = "";
                    if (entry->name_length > 0 && entry->name_offset + entry->name_length * 2 <= entry->record_length) {
                        const uint16_t* name_ptr = reinterpret_cast<const uint16_t*>(
                            reinterpret_cast<const uint8_t*>(entry) + entry->name_offset
                        );
                        entry_name = utf16le_to_utf8(name_ptr, entry->name_length);
                    }
                    if (entry_name == "$J") {
                        uint64_t ext_mft_idx = entry->mft_reference & 0x0000FFFFFFFFFFFFULL;
                        std::vector<uint8_t> ext_rec_buf;
                        if (read_mft_record(ext_mft_idx, ext_rec_buf)) {
                            MFT_REC* ext_rec = reinterpret_cast<MFT_REC*>(ext_rec_buf.data());
                            if (apply_fixups(ext_rec_buf.data(), record_size_, ext_rec->rhdr.fix_off, ext_rec->rhdr.fix_num)) {
                                uint32_t ext_attr_offset = ext_rec->attr_off;
                                while (ext_attr_offset + 24 <= ext_rec->used) {
                                    const ATTRIB* ext_attrib = reinterpret_cast<const ATTRIB*>(ext_rec_buf.data() + ext_attr_offset);
                                    if (ext_attrib->type == ATTR_END) break;
                                    if (ext_attr_offset + ext_attrib->size > ext_rec->used) break;

                                    if (ext_attrib->type == ATTR_DATA) {
                                        std::string ext_name = "";
                                        if (ext_attrib->name_len > 0) {
                                            const uint16_t* ext_name_ptr = reinterpret_cast<const uint16_t*>(
                                                reinterpret_cast<const uint8_t*>(ext_attrib) + ext_attrib->name_off
                                            );
                                            ext_name = utf16le_to_utf8(ext_name_ptr, ext_attrib->name_len);
                                        }
                                        if (ext_name == "$J" && ext_attrib->id == entry->attribute_id) {
                                            if (ext_attrib->non_res) {
                                                return ext_attrib->nres.data_size;
                                            }
                                        }
                                    }
                                    ext_attr_offset += ext_attrib->size;
                                    if (ext_attrib->size == 0) break;
                                }
                            }
                        }
                    }
                }
                al_offset += entry->record_length;
            }
        }
    }
    return 0;
}

bool NtfsParser::update_index_incremental() {
    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<UsnJournalEntry> entries;
    uint64_t next_usn = last_usn_;
    if (!parse_usn_journal(entries, last_usn_, &next_usn)) {
        return false;
    }

    if (entries.empty()) {
        std::cout << "No new changes in USN Change Journal. Index is up to date.\n";
        return true;
    }

    size_t added = 0;
    size_t modified = 0;
    size_t deleted = 0;
    std::unordered_map<uint64_t, std::string> deleted_names;
    std::unordered_map<uint64_t, std::string> updated_names;

    for (const auto& entry : entries) {
        if (entry.reason & 0x00000200) { // DELETE
            auto it = files_.find(entry.file_id);
            if (it != files_.end()) {
                deleted_names[entry.file_id] = it->second.full_path;
                files_.erase(it);
                deleted++;
            }
            updated_names.erase(entry.file_id);
        } else {
            FileEntry file_entry;
            bool exists_before = (files_.find(entry.file_id) != files_.end());
            std::string old_path = exists_before ? files_[entry.file_id].full_path : "";

            if (parse_mft_record_to_entry(entry.file_id, file_entry)) {
                files_[entry.file_id] = file_entry;
                if (exists_before) {
                    modified++;
                    updated_names[entry.file_id] = "Modified: " + old_path;
                } else {
                    added++;
                    updated_names[entry.file_id] = "Added: " + file_entry.name;
                }
            } else {
                auto it = files_.find(entry.file_id);
                if (it != files_.end()) {
                    deleted_names[entry.file_id] = it->second.full_path;
                    files_.erase(it);
                    deleted++;
                }
                updated_names.erase(entry.file_id);
            }
        }
    }

    resolve_all_paths();

    std::cout << "\nIncremental Update Summary:\n";
    std::cout << fmt::format("  - Files Added:    {}\n", added);
    std::cout << fmt::format("  - Files Modified: {}\n", modified);
    std::cout << fmt::format("  - Files Deleted:  {}\n", deleted);

    std::cout << "\nDetail of changes (last 50):\n";
    size_t print_count = 0;
    for (const auto& [id, path] : deleted_names) {
        if (print_count >= 50) break;
        std::cout << fmt::format("  [DELETED]  {}\n", path);
        print_count++;
    }
    for (const auto& [id, msg] : updated_names) {
        if (print_count >= 50) break;
        auto it = files_.find(id);
        if (it != files_.end()) {
            if (msg.rfind("Modified:", 0) == 0) {
                if (msg != "Modified: " + it->second.full_path) {
                    std::cout << fmt::format("  [RENAMED]   {} -> {}\n", msg.substr(10), it->second.full_path);
                } else {
                    std::cout << fmt::format("  [MODIFIED]  {}\n", it->second.full_path);
                }
            } else {
                std::cout << fmt::format("  [ADDED]     {}\n", it->second.full_path);
            }
            print_count++;
        }
    }

    last_usn_ = next_usn;
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << fmt::format("\nIncremental update finished in {} ms. Current USN: 0x{:X}\n\n", 
        elapsed.count(), last_usn_);

    return true;
}

