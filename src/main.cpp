#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fmt/format.h>
#include "ntfs_parser.h"

namespace {

std::string to_lowercase(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return lower;
}

void print_help(const char* prog_name) {
    std::cout << fmt::format("Usage: {} <device_or_image_path>\n", prog_name);
    std::cout << "Example:\n";
    std::cout << fmt::format("  {} /dev/sdb1        # Scan a physical NTFS partition (requires root)\n", prog_name);
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

    std::cout << "Starting MFT table scan...\n";
    if (!parser.parse()) {
        std::cerr << "Error: Parsing failed.\n";
        return 1;
    }

    parser.print_stats();

    const auto& files = parser.get_files();

    std::cout << "======================================================\n";
    std::cout << "Interactive Search Mode Enabled!\n";
    std::cout << "  - Enter search query to find files (case-insensitive substring match)\n";
    std::cout << "  - Type ':stats' to show scanning statistics\n";
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
            parser.print_stats();
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
