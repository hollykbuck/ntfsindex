#pragma once
#include "ntfs_indexer.h"
#include "ntfs_parser.h"

class TuiClient {
public:
    TuiClient(NtfsParser& parser, NtfsIndexer& indexer, const std::string& dev_path, const std::string& cache_file = "");
    ~TuiClient() = default;

    // Start the TUI interactive loop
    void run();

private:
    NtfsParser& parser_;
    NtfsIndexer& indexer_;
    std::string dev_path_;
    std::string cache_file_;
};
