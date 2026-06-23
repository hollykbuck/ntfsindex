#include "ntfs_indexer.h"
#include <iostream>
#include "absl/log/log.h"
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <fstream>

void to_json(nlohmann::json& j, const FileEntry& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"parent_id", e.parent_id},
        {"name", e.name},
        {"is_directory", e.is_directory},
        {"size", e.size}
    };
}

void from_json(const nlohmann::json& j, FileEntry& e) {
    j.at("id").get_to(e.id);
    j.at("parent_id").get_to(e.parent_id);
    j.at("name").get_to(e.name);
    j.at("is_directory").get_to(e.is_directory);
    j.at("size").get_to(e.size);
}

namespace {
std::string format_size(uint64_t bytes) {
    const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
    int suffix_idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && suffix_idx < 4) {
        size /= 1024.0;
        suffix_idx++;
    }
    return fmt::format("{:.2f} {}", size, suffixes[suffix_idx]);
}
} // namespace

NtfsIndexer::NtfsIndexer() = default;
NtfsIndexer::~NtfsIndexer() = default;

bool NtfsIndexer::build_initial_index(NtfsParser& parser, std::function<void(uint64_t processed, uint64_t total)> progress_cb) {
    auto start_time = std::chrono::high_resolution_clock::now();

    uint64_t num_records = parser.mft_record_count();
    files_.reserve(num_records);

    constexpr uint64_t CHUNK_RECORDS = 4096; // Read 4096 records (4MB) at a time
    const uint64_t record_size = parser.record_size();
    std::vector<uint8_t> chunk_buf(CHUNK_RECORDS * record_size);

    for (uint64_t start_idx = 0; start_idx < num_records; start_idx += CHUNK_RECORDS) {
        uint64_t count = std::min(CHUNK_RECORDS, num_records - start_idx);
        
        if (!parser.read_mft_records_bulk(start_idx, count, chunk_buf.data())) {
            LOG(WARNING) << fmt::format("[Scan] Failed to bulk read MFT records from index {} to {}. Falling back to individual reads.", start_idx, start_idx + count - 1);
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t idx = start_idx + i;
                FileEntry entry;
                if (parser.parse_mft_record_to_entry(idx, entry)) {
                    files_[idx] = entry;
                }
            }
        } else {
            // Process the records in memory
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t idx = start_idx + i;
                uint8_t* record_data = chunk_buf.data() + i * record_size;
                FileEntry entry;
                if (parser.parse_mft_record_to_entry(idx, record_data, entry)) {
                    files_[idx] = entry;
                }
            }
        }

        if (progress_cb) {
            uint64_t processed = std::min(start_idx + count, num_records);
            progress_cb(processed, num_records);
        }
    }

    // Query and initialize last USN journal position
    uint64_t usn_mft_idx = 0;
    for (const auto& [id, entry] : files_) {
        if (entry.parent_id == 11 && entry.name == "$UsnJrnl") {
            usn_mft_idx = id;
            break;
        }
    }
    last_usn_ = parser.query_current_usn(usn_mft_idx);
    LOG(INFO) << fmt::format("[USN Init] Initialized last USN position to: 0x{:X}", last_usn_);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    LOG(INFO) << fmt::format("[Scan Completed] Scanned {} files/dirs in {} ms", files_.size(), elapsed.count());

    return true;
}

bool NtfsIndexer::update_index_incremental(NtfsParser& parser) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // Find usn_mft_idx from the file tree
    uint64_t usn_mft_idx = 0;
    for (const auto& [id, entry] : files_) {
        if (entry.parent_id == 11 && entry.name == "$UsnJrnl") {
            usn_mft_idx = id;
            break;
        }
    }

    std::vector<NtfsParser::UsnJournalEntry> entries;
    uint64_t next_usn = last_usn_;
    if (!parser.parse_usn_journal(entries, usn_mft_idx, last_usn_, &next_usn)) {
        LOG(ERROR) << "[Indexer] Error: Failed to parse USN Journal during incremental update.";
        return false;
    }

    if (entries.empty()) {
        LOG(INFO) << "No new changes in USN Change Journal. Index is up to date.";
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
                deleted_names[entry.file_id] = get_absolute_path(entry.file_id);
                files_.erase(it);
                deleted++;
            }
            updated_names.erase(entry.file_id);
        } else {
            FileEntry file_entry;
            bool exists_before = (files_.find(entry.file_id) != files_.end());
            std::string old_path = exists_before ? get_absolute_path(entry.file_id) : "";

            if (parser.parse_mft_record_to_entry(entry.file_id, file_entry)) {
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
                    deleted_names[entry.file_id] = get_absolute_path(entry.file_id);
                    files_.erase(it);
                    deleted++;
                }
                updated_names.erase(entry.file_id);
            }
        }
    }

    LOG(INFO) << "Incremental Update Summary:";
    LOG(INFO) << fmt::format("  - Files Added:    {}", added);
    LOG(INFO) << fmt::format("  - Files Modified: {}", modified);
    LOG(INFO) << fmt::format("  - Files Deleted:  {}", deleted);

    LOG(INFO) << "Detail of changes (last 50):";
    size_t print_count = 0;
    for (const auto& [id, path] : deleted_names) {
        if (print_count >= 50) break;
        LOG(INFO) << fmt::format("  [DELETED]  {}", path);
        print_count++;
    }
    for (const auto& [id, msg] : updated_names) {
        if (print_count >= 50) break;
        auto it = files_.find(id);
        if (it != files_.end()) {
            std::string current_path = get_absolute_path(id);
            if (msg.rfind("Modified:", 0) == 0) {
                if (msg != "Modified: " + current_path) {
                    LOG(INFO) << fmt::format("  [RENAMED]   {} -> {}", msg.substr(10), current_path);
                } else {
                    LOG(INFO) << fmt::format("  [MODIFIED]  {}", current_path);
                }
            } else {
                LOG(INFO) << fmt::format("  [ADDED]     {}", current_path);
            }
            print_count++;
        }
    }

    last_usn_ = next_usn;
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    LOG(INFO) << fmt::format("Incremental update finished in {} ms. Current USN: 0x{:X}", 
        elapsed.count(), last_usn_);

    return true;
}

std::string NtfsIndexer::get_absolute_path(uint64_t id) const {
    if (id == 5) {
        return "/";
    }

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
            path += "/" + files_.at(*r_it).name;
        }
    } else {
        // Orphan path
        path = "/[orphan]";
        for (auto r_it = path_ids.rbegin(); r_it != path_ids.rend(); ++r_it) {
            path += "/" + files_.at(*r_it).name;
        }
    }
    
    return path;
}

void NtfsIndexer::print_stats(const std::string& dev_path) const {
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

    LOG(INFO) << "================ NTFS Scan Statistics ================";
    LOG(INFO) << fmt::format("Device Path:         {}", dev_path);
    LOG(INFO) << fmt::format("Total Directories:   {}", dir_count);
    LOG(INFO) << fmt::format("Total Files:         {}", file_count);
    LOG(INFO) << fmt::format("Total Logical Size:  {} ({})", total_bytes, format_size(total_bytes));
    LOG(INFO) << "======================================================";
}

bool NtfsIndexer::save_to_cache(const std::string& cache_path) const {
    try {
        std::ofstream os(cache_path, std::ios::binary);
        if (!os.is_open()) {
            LOG(ERROR) << "[Cache] Failed to open cache file for writing: " << cache_path;
            return false;
        }
        
        nlohmann::json j;
        j["last_usn"] = last_usn_;
        j["files"] = files_;
        
        std::vector<uint8_t> msgpack = nlohmann::json::to_msgpack(j);
        os.write(reinterpret_cast<const char*>(msgpack.data()), msgpack.size());
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR) << "[Cache] Exception while saving cache: " << e.what();
        return false;
    } catch (...) {
        LOG(ERROR) << "[Cache] Unknown exception while saving cache.";
        return false;
    }
}

bool NtfsIndexer::load_from_cache(const std::string& cache_path) {
    try {
        std::ifstream is(cache_path, std::ios::binary | std::ios::ate);
        if (!is.is_open()) {
            return false;
        }
        
        std::streamsize size = is.tellg();
        is.seekg(0, std::ios::beg);
        
        std::vector<uint8_t> buffer(size);
        if (!is.read(reinterpret_cast<char*>(buffer.data()), size)) {
            LOG(ERROR) << "[Cache] Failed to read cache file content: " << cache_path;
            return false;
        }
        
        nlohmann::json j = nlohmann::json::from_msgpack(buffer);
        last_usn_ = j.at("last_usn").get<uint64_t>();
        files_ = j.at("files").get<std::unordered_map<uint64_t, FileEntry>>();
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR) << "[Cache] Exception while loading cache: " << e.what();
        return false;
    } catch (...) {
        LOG(ERROR) << "[Cache] Unknown exception while loading cache.";
        return false;
    }
}
