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
#include "http_server.h"

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
    std::cout << fmt::format("Usage: {} <mft_file_or_partition_path> [port] [doc_root]\n", prog_name);
    std::cout << "Examples:\n";
    std::cout << fmt::format("  {} $MFT              # Scan MFT file and serve on http://127.0.0.1:8080/\n", prog_name);
    std::cout << fmt::format("  {} \\\\.\\C: 9000       # Scan C: drive and serve on http://127.0.0.1:9000/\n", prog_name);
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

    unsigned short port = 8080;
    if (argc >= 3) {
        try {
            port = static_cast<unsigned short>(std::stoul(argv[2]));
        } catch (...) {
            std::cerr << "Invalid port number specified. Using default 8080.\n";
        }
    }

    std::string doc_root = "./web";
    if (argc >= 4) {
        doc_root = argv[3];
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

    std::string address = "0.0.0.0"; // Bind to all network interfaces
    HttpServer server(parser, indexer, address, port, doc_root, dev_path);
    if (!server.run()) {
        return 1;
    }

    return 0;
}
