#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fmt/format.h>
#include "ntfs_parser.h"
#include "ntfs_indexer.h"

namespace {

std::string to_lowercase(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return lower;
}

std::string format_usn_reason(uint32_t reason) {
    std::vector<std::string> parts;
    if (reason & 0x00000100) parts.push_back("CREATE");
    if (reason & 0x00000200) parts.push_back("DELETE");
    if (reason & 0x00001000) parts.push_back("RENAME_OLD");
    if (reason & 0x00002000) parts.push_back("RENAME_NEW");
    if (reason & 0x00000001) parts.push_back("DATA_OVERWRITE");
    if (reason & 0x00000002) parts.push_back("DATA_EXTEND");
    if (reason & 0x00000004) parts.push_back("DATA_TRUNC");
    if (reason & 0x00008000) parts.push_back("BASIC_INFO");
    if (reason & 0x00000800) parts.push_back("SECURITY");
    if (reason & 0x80000000) parts.push_back("CLOSE");
    
    if (parts.empty() && reason != 0) {
        parts.push_back(fmt::format("0x{:X}", reason));
    }
    
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "|";
        result += parts[i];
    }
    return result.empty() ? "NONE" : result;
}

std::string format_filetime(uint64_t filetime) {
    if (filetime == 0) return "-";
    // Convert 100-nanosecond intervals to seconds (Win32 Epoch to Unix Epoch)
    uint64_t unix_time = (filetime / 10000000ULL) - 11644473600ULL;
    std::time_t t = static_cast<std::time_t>(unix_time);
    std::tm tm;
    #ifdef _WIN32
    gmtime_s(&tm, &t);
    #else
    gmtime_r(&t, &tm);
    #endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return std::string(buf);
}

void print_help(const char* prog_name) {
    std::cout << fmt::format("Usage: {} <mft_file_or_partition_path>\n", prog_name);
    std::cout << "Examples:\n";
    std::cout << fmt::format("  {} $MFT              # Scan an exported raw $MFT binary file\n", prog_name);
    std::cout << fmt::format("  {} \\\\.\\C:           # Scan Windows C: drive directly (requires Administrator privileges)\n", prog_name);
    std::cout << fmt::format("  {} /dev/sdb1         # Scan a Linux physical NTFS partition (requires root)\n", prog_name);
    std::cout << fmt::format("  {} test_ntfs.img     # Scan a backup/test NTFS image file\n", prog_name);
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    std::string dev_path = argv[1];
    if (dev_path == "-h" || dev_path == "--help") {
        print_help(argv[0]);
        return 0;
    }

    std::cout << fmt::format("Initializing parser for device: {} ...\n", dev_path);
    NtfsParser parser;
    if (!parser.init(dev_path)) {
        return 1;
    }

    std::cout << "Starting MFT metadata parse...\n";
    if (!parser.parse()) {
        std::cerr << "Error: MFT parsing failed.\n";
        return 1;
    }

    std::cout << "Building initial file index...\n";
    NtfsIndexer indexer;
    if (!indexer.build_initial_index(parser)) {
        std::cerr << "Error: Index build failed.\n";
        return 1;
    }

    indexer.print_stats(dev_path);

    const auto& files = indexer.get_files();

    std::cout << "======================================================\n";
    std::cout << "Interactive Search Mode Enabled!\n";
    std::cout << "  - Enter search query to find files (case-insensitive substring match)\n";
    std::cout << "  - Type ':stats' to show scanning statistics\n";
    std::cout << "  - Type ':usn' to parse and view USN Change Journal ($UsnJrnl)\n";
    std::cout << "  - Type ':update' to perform incremental index update\n";
    std::cout << "  - Type ':exit' or ':q' to exit\n";
    std::cout << "======================================================\n\n";

    std::string query;
    while (true) {
        std::cout << "search> ";
        if (!std::getline(std::cin, query)) {
            break;
        }

        // Trim input
        query.erase(0, query.find_first_not_of(" \t\r\n"));
        query.erase(query.find_last_not_of(" \t\r\n") + 1);

        if (query.empty()) {
            continue;
        }

        if (query == ":exit" || query == ":q" || query == "exit" || query == "quit") {
            std::cout << "Goodbye!\n";
            break;
        }

        if (query == ":stats") {
            indexer.print_stats(dev_path);
            continue;
        }

        if (query == ":usn") {
            // Find usn_mft_idx from the file tree
            uint64_t usn_mft_idx = 0;
            for (const auto& [id, entry] : files) {
                if (entry.parent_id == 11 && entry.name == "$UsnJrnl") {
                    usn_mft_idx = id;
                    break;
                }
            }

            if (usn_mft_idx == 0) {
                std::cout << "USN Change Journal ($UsnJrnl) is not active on this volume.\n\n";
                continue;
            }

            std::cout << "Parsing USN Change Journal ($J stream)...\n";
            std::vector<NtfsParser::UsnJournalEntry> usn_entries;
            auto usn_start = std::chrono::high_resolution_clock::now();
            bool success = parser.parse_usn_journal(usn_entries, usn_mft_idx);
            auto usn_end = std::chrono::high_resolution_clock::now();
            auto usn_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(usn_end - usn_start);

            if (!success) {
                std::cout << "USN Change Journal stream could not be parsed on this volume.\n\n";
            } else if (usn_entries.empty()) {
                std::cout << fmt::format("USN Change Journal parsed successfully in {} ms, but no active records were found (it might be empty or fully truncated).\n\n", usn_elapsed.count());
            } else {
                std::cout << fmt::format("Found {} USN change records in {} ms:\n", usn_entries.size(), usn_elapsed.count());
                std::cout << fmt::format("{:<18} | {:<16} | {:<25} | {:<30} | {}\n", "USN", "File ID", "Timestamp", "Reason", "Filename");
                std::cout << std::string(110, '-') << "\n";
                // Show last 100 entries by default to avoid spamming the terminal
                size_t start_idx = usn_entries.size() > 100 ? usn_entries.size() - 100 : 0;
                if (start_idx > 0) {
                    std::cout << fmt::format("... [Skipped first {} records] ...\n", start_idx);
                }
                for (size_t i = start_idx; i < usn_entries.size(); ++i) {
                    const auto& entry = usn_entries[i];
                    std::cout << fmt::format("0x{:016X} | {:<16} | {:<25} | {:<30} | {}\n",
                                             entry.usn,
                                             entry.file_id,
                                             format_filetime(entry.timestamp),
                                             format_usn_reason(entry.reason),
                                             entry.filename);
                }
                std::cout << "\n";
            }
            continue;
        }

        if (query == ":update") {
            indexer.update_index_incremental(parser);
            continue;
        }

        std::string query_lower = to_lowercase(query);
        std::vector<const FileEntry*> matches;

        auto search_start = std::chrono::high_resolution_clock::now();
        
        for (const auto& [id, entry] : files) {
            std::string name_lower = to_lowercase(entry.name);
            if (name_lower.find(query_lower) != std::string::npos) {
                matches.push_back(&entry);
            }
        }

        auto search_end = std::chrono::high_resolution_clock::now();
        auto search_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(search_end - search_start);

        // Sort matches alphabetically by path
        std::sort(matches.begin(), matches.end(), [](const FileEntry* a, const FileEntry* b) {
            return a->full_path < b->full_path;
        });

        // Print matches (limit to 50 results)
        size_t display_count = std::min(matches.size(), size_t(50));
        if (matches.empty()) {
            std::cout << fmt::format("No matches found for '{}' (searched in {} us)\n\n", query, search_elapsed.count());
        } else {
            std::cout << fmt::format("Found {} matches (showing first {} matches) in {} us:\n", 
                                     matches.size(), display_count, search_elapsed.count());
            std::cout << fmt::format("{:<70} | {:<10} | {}\n", "Path", "Type", "Size");
            std::cout << std::string(100, '-') << "\n";
            
            for (size_t i = 0; i < display_count; ++i) {
                const auto* entry = matches[i];
                std::string type_str = entry->is_directory ? "DIR" : "FILE";
                std::string size_str = entry->is_directory ? "-" : fmt::format("{}", entry->size);
                
                // Truncate path if too long
                std::string path_to_show = entry->full_path;
                if (path_to_show.length() > 68) {
                    path_to_show = "..." + path_to_show.substr(path_to_show.length() - 65);
                }
                
                std::cout << fmt::format("{:<70} | {:<10} | {}\n", path_to_show, type_str, size_str);
            }
            std::cout << "\n";
        }
    }

    return 0;
}
