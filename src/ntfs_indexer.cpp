#include "ntfs_indexer.h"
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <fmt/format.h>

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

bool NtfsIndexer::build_initial_index(NtfsParser& parser) {
    auto start_time = std::chrono::high_resolution_clock::now();

    uint64_t num_records = parser.mft_record_count();
    files_.reserve(num_records);

    for (uint64_t idx = 0; idx < num_records; ++idx) {
        FileEntry entry;
        if (parser.parse_mft_record_to_entry(idx, entry)) {
            files_[idx] = entry;
        }
    }

    // Resolve all parent-child full paths
    resolve_all_paths();

    // Query and initialize last USN journal position
    uint64_t usn_mft_idx = 0;
    for (const auto& [id, entry] : files_) {
        if (entry.parent_id == 11 && entry.name == "$UsnJrnl") {
            usn_mft_idx = id;
            break;
        }
    }
    last_usn_ = parser.query_current_usn(usn_mft_idx);
    std::cout << fmt::format("[USN Init] Initialized last USN position to: 0x{:X}\n", last_usn_);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << fmt::format("[Scan Completed] Scanned {} files/dirs in {} ms\n", files_.size(), elapsed.count());

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

void NtfsIndexer::resolve_all_paths() {
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

    std::cout << "\n================ NTFS Scan Statistics ================\n";
    std::cout << fmt::format("Device Path:         {}\n", dev_path);
    std::cout << fmt::format("Total Directories:   {}\n", dir_count);
    std::cout << fmt::format("Total Files:         {}\n", file_count);
    std::cout << fmt::format("Total Logical Size:  {} ({})\n", total_bytes, format_size(total_bytes));
    std::cout << "======================================================\n\n";
}
