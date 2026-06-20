#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include "ntfs_parser.h"
#include "ntfs_indexer.h"
#include "http_server.h"
#include "tui_client.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"

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

struct AppConfig {
    std::string device_path = "$MFT";
    uint16_t port = 8080;
    std::string address = "0.0.0.0";
    std::string doc_root = "./web";
};

AppConfig load_config(const std::string& path) {
    AppConfig config;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cout << "[Config] Configuration file not found or couldn't be opened: " << path << ". Using defaults.\n";
        return config;
    }
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("device_path") && j["device_path"].is_string()) {
            config.device_path = j["device_path"].get<std::string>();
        }
        if (j.contains("port") && j["port"].is_number_integer()) {
            config.port = j["port"].get<uint16_t>();
        }
        if (j.contains("address") && j["address"].is_string()) {
            config.address = j["address"].get<std::string>();
        }
        if (j.contains("doc_root") && j["doc_root"].is_string()) {
            config.doc_root = j["doc_root"].get<std::string>();
        }
        std::cout << "[Config] Loaded configuration from: " << path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[Config] Error parsing " << path << ": " << e.what() << ". Using defaults.\n";
    }
    return config;
}

} // namespace

// Define Abseil command line flags
ABSL_FLAG(std::string, device_path, "", "Path to raw MFT file or partition (e.g. \\\\.\\C: or $MFT)");
ABSL_FLAG(uint16_t, port, 0, "Port for the HTTP API server (e.g. 8080)");
ABSL_FLAG(std::string, address, "", "IP address to bind the HTTP server to (e.g. 0.0.0.0)");
ABSL_FLAG(std::string, doc_root, "", "Document root directory serving frontend assets");
ABSL_FLAG(std::string, config, "config.json", "Path to config.json file containing default options");
ABSL_FLAG(bool, tui, false, "Start in interactive TUI mode instead of HTTP Server");

int main(int argc, char* argv[]) {
    // Initialize Abseil Program Usage and Parse CommandLine
    absl::SetProgramUsageMessage("NTFS Indexer and HTTP Search Server. Start using config.json, arguments, or flags.");
    auto positional_args = absl::ParseCommandLine(argc, argv);

    // 1. Load config file
    std::string config_path = absl::GetFlag(FLAGS_config);
    AppConfig config = load_config(config_path);

    // 2. Override with legacy positional arguments if provided
    // Usage: prog_name <device_path> [port] [doc_root]
    if (positional_args.size() >= 2) {
        config.device_path = positional_args[1];
    }
    if (positional_args.size() >= 3) {
        try {
            config.port = static_cast<uint16_t>(std::stoul(positional_args[2]));
        } catch (...) {}
    }
    if (positional_args.size() >= 4) {
        config.doc_root = positional_args[3];
    }

    // 3. Override with explicit Abseil flags if specified on CLI
    if (!absl::GetFlag(FLAGS_device_path).empty()) {
        config.device_path = absl::GetFlag(FLAGS_device_path);
    }
    if (absl::GetFlag(FLAGS_port) != 0) {
        config.port = absl::GetFlag(FLAGS_port);
    }
    if (!absl::GetFlag(FLAGS_address).empty()) {
        config.address = absl::GetFlag(FLAGS_address);
    }
    if (!absl::GetFlag(FLAGS_doc_root).empty()) {
        config.doc_root = absl::GetFlag(FLAGS_doc_root);
    }

    std::cout << "---------------------------------------------\n";
    std::cout << "[Server Config Options]\n";
    std::cout << fmt::format("  - Device/MFT Path: {}\n", config.device_path);
    std::cout << fmt::format("  - Listen Address:  {}\n", config.address);
    std::cout << fmt::format("  - Listen Port:     {}\n", config.port);
    std::cout << fmt::format("  - Frontend Root:   {}\n", config.doc_root);
    std::cout << "---------------------------------------------\n";

    std::cout << fmt::format("Initializing parser for device: {} ...\n", config.device_path);
    NtfsParser parser;
    if (!parser.init(config.device_path)) {
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

    indexer.print_stats(config.device_path);

    if (absl::GetFlag(FLAGS_tui)) {
        TuiClient tui(parser, indexer);
        tui.run();
    } else {
        HttpServer server(parser, indexer, config.address, config.port, config.doc_root, config.device_path);
        if (!server.run()) {
            return 1;
        }
    }

    return 0;
}
