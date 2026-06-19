#include "ntfs_parser.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <fmt/format.h>

namespace {

// Helper to convert UTF-16LE to UTF-8
std::string utf16le_to_utf8(const uint16_t* utf16, size_t len) {
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

NtfsParser::NtfsParser() {}

NtfsParser::~NtfsParser() {
    if (fd_ != -1) {
        ::close(fd_);
    }
}

bool NtfsParser::init(const std::string& dev_path) {
    dev_path_ = dev_path;
    fd_ = ::open(dev_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ == -1) {
        std::cerr << fmt::format("Error: Failed to open device/file '{}': {}\n", dev_path_, std::strerror(errno));
        return false;
    }

    // Read Boot Sector (VBR)
    std::vector<uint8_t> boot_buf(512);
    if (!read_disk(0, boot_buf.data(), 512)) {
        std::cerr << "Error: Failed to read Boot Sector\n";
        return false;
    }

    const NTFS_BOOT* boot = reinterpret_cast<const NTFS_BOOT*>(boot_buf.data());
    if (std::memcmp(boot->system_id, "NTFS    ", 8) != 0) {
        std::cerr << "Error: Device does not contain a valid NTFS filesystem signature.\n";
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
    if (fd_ == -1) return false;
    
    if (lseek(fd_, offset, SEEK_SET) == -1) {
        return false;
    }

    uint8_t* ptr = reinterpret_cast<uint8_t*>(buffer);
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t bytes_read = ::read(fd_, ptr + total_read, size - total_read);
        if (bytes_read <= 0) {
            if (bytes_read < 0 && errno == EINTR) continue;
            return false;
        }
        total_read += bytes_read;
    }
    return true;
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

    // 1. Read MFT Record 0 ($MFT itself)
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

    uint64_t num_records = mft_total_size_ / record_size_;
    std::cout << fmt::format("[MFT Parse] MFT Total Size: {} ({})\n", mft_total_size_, format_size(mft_total_size_));
    std::cout << fmt::format("[MFT Parse] MFT Records Count: {}\n", num_records);

    // 2. Loop through all MFT records
    std::vector<uint8_t> rec_buf(record_size_);
    files_.reserve(num_records);

    for (uint64_t idx = 0; idx < num_records; ++idx) {
        // Read record
        if (!read_mft_record(idx, rec_buf)) {
            continue; // Skip failed reads
        }

        MFT_REC* rec = reinterpret_cast<MFT_REC*>(rec_buf.data());

        // Perform Fixups
        if (!apply_fixups(rec_buf.data(), record_size_, rec->rhdr.fix_off, rec->rhdr.fix_num)) {
            continue; // Fixup failed, record corrupted, skip
        }

        // Validate record
        if (rec->rhdr.sign != SIGN_FILE) {
            continue; // Skip invalid signatures
        }

        // Only parse active base MFT records
        if (!(rec->flags & RECORD_FLAG_IN_USE)) {
            continue; // Skip unused/deleted records
        }

        if (rec->parent_ref.get_record_id() != 0) {
            continue; // Skip non-base/extension records
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
            FileEntry entry;
            entry.id = idx;
            entry.parent_id = parent_id;
            entry.name = best_name;
            entry.is_directory = (rec->flags & RECORD_FLAG_DIR);
            entry.size = entry.is_directory ? 0 : file_size;
            files_[idx] = entry;
        }
    }

    // 3. Resolve paths
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

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << fmt::format("[Scan Completed] Scanned {} files/dirs in {} ms\n", files_.size(), elapsed.count());

    return true;
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
