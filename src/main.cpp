#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <fmt/format.h>
#include "ntfs_parser.h"
#include "ntfs_indexer.h"
#include "http_server.h"
#include "tui_client.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/log.h"
#include "absl/log/initialize.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/log/globals.h"
#include "absl/base/log_severity.h"
#include <mutex>
#include <cstdlib>
#include <memory>
#include <stdexec/execution.hpp>
#include <exec/single_thread_context.hpp>
#include <exec/asio/asio_thread_pool.hpp>
#include <exec/asio/use_sender.hpp>
#include <exec/async_scope.hpp>
#include <utility>

#include <boost/beast/http.hpp>
#include <boost/beast/core.hpp>


using SchedulerType = decltype(std::declval<exec::single_thread_context>().get_scheduler());


namespace {

class FileLogSink : public absl::LogSink {
 public:
  explicit FileLogSink(const std::string& filename) : file_(filename, std::ios::app) {}
  ~FileLogSink() override {
      std::lock_guard<std::mutex> lock(mutex_);
      if (file_.is_open()) {
          file_.close();
      }
  }

  void Send(const absl::LogEntry& entry) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
      file_ << entry.text_message_with_prefix_and_newline();
      file_.flush();
    }
  }

 private:
  std::ofstream file_;
  std::mutex mutex_;
};

class ScopedFileLogger {
public:
    explicit ScopedFileLogger(const std::string& filename) {
        sink_ = std::make_unique<FileLogSink>(filename);
        absl::AddLogSink(sink_.get());
    }
    ~ScopedFileLogger() {
        if (sink_) {
            absl::RemoveLogSink(sink_.get());
        }
    }
private:
    std::unique_ptr<FileLogSink> sink_;
};

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
    uint32_t auto_update_interval = 0;
};

} // namespace

// Define Abseil command line flags
ABSL_FLAG(std::string, device_path, "$MFT", "Path to raw MFT file or partition (e.g. \\\\.\\C: or $MFT)");
ABSL_FLAG(uint16_t, port, 8080, "Port for the HTTP API server (e.g. 8080)");
ABSL_FLAG(std::string, address, "0.0.0.0", "IP address to bind the HTTP server to (e.g. 0.0.0.0)");
ABSL_FLAG(std::string, doc_root, "./web", "Document root directory serving frontend assets");
ABSL_FLAG(bool, tui, false, "Start in interactive TUI mode instead of HTTP Server");
ABSL_FLAG(uint32_t, auto_update_interval, 0, "Interval in seconds for automatic incremental updates (0 to disable)");

int main(int argc, char* argv[]) {
    // Initialize Abseil Program Usage and Parse CommandLine
    absl::SetProgramUsageMessage("NTFS Indexer and HTTP Search Server. Start using flags or standard flagfile (e.g. --flagfile=flags.txt).");
    absl::ParseCommandLine(argc, argv);

    // Initialize Abseil Logging
    absl::InitializeLog();

    // Set up file logging if environment variable is set
    std::unique_ptr<ScopedFileLogger> file_logger;
    const char* log_file_env = std::getenv("NTFSINDEX_LOG_FILE");
    if (log_file_env && log_file_env[0] != '\0') {
        file_logger = std::make_unique<ScopedFileLogger>(log_file_env);
    }

    // Silence stderr logging in TUI mode to avoid terminal corruption
    if (absl::GetFlag(FLAGS_tui)) {
        absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
    }

    // Load config options from Abseil flags
    AppConfig config;
    config.device_path = absl::GetFlag(FLAGS_device_path);
    config.port = absl::GetFlag(FLAGS_port);
    config.address = absl::GetFlag(FLAGS_address);
    config.doc_root = absl::GetFlag(FLAGS_doc_root);
    config.auto_update_interval = absl::GetFlag(FLAGS_auto_update_interval);

    LOG(INFO) << "---------------------------------------------";
    LOG(INFO) << "[Server Config Options]";
    LOG(INFO) << fmt::format("  - Device/MFT Path: {}", config.device_path);
    LOG(INFO) << fmt::format("  - Listen Address:  {}", config.address);
    LOG(INFO) << fmt::format("  - Listen Port:     {}", config.port);
    LOG(INFO) << fmt::format("  - Frontend Root:   {}", config.doc_root);
    LOG(INFO) << fmt::format("  - Auto Update:     {}s", config.auto_update_interval == 0 ? "Disabled" : std::to_string(config.auto_update_interval));
    LOG(INFO) << "---------------------------------------------";

    exec::single_thread_context worker_ctx;
    auto scheduler = worker_ctx.get_scheduler();

    exec::asio::asio_thread_pool io_pool(2);
    auto io_scheduler = io_pool.get_scheduler();

    NtfsParser parser;

    NtfsIndexer indexer;

    if (absl::GetFlag(FLAGS_tui)) {
        TuiClient tui(parser, indexer, config.device_path);
        tui.run();
    } else {
        auto init_sender = stdexec::schedule(scheduler)
            | stdexec::then([&]() -> bool {
                LOG(INFO) << fmt::format("Initializing parser for device: {} ...", config.device_path);
                if (!parser.init(config.device_path)) {
                    return false;
                }

                LOG(INFO) << "Starting MFT metadata parse...";
                if (!parser.parse()) {
                    LOG(ERROR) << "Error: MFT parsing failed.";
                    return false;
                }

                LOG(INFO) << "Building initial file index...";
                if (!indexer.build_initial_index(parser)) {
                    LOG(ERROR) << "Error: Index build failed.";
                    return false;
                }

                return true;
            });

        auto init_result = stdexec::sync_wait(std::move(init_sender));
        if (!init_result || !std::get<0>(*init_result)) {
            return 1;
        }

        indexer.print_stats(config.device_path);

        HttpServer server(parser, indexer, scheduler, io_scheduler, config.address, config.port, config.doc_root, config.device_path, config.auto_update_interval);
        if (!server.run()) {
            return 1;
        }
    }

    return 0;
}
