#pragma once
#include "ntfs_indexer.h"
#include "ntfs_parser.h"

class TuiClient {
public:
    TuiClient(NtfsParser& parser, NtfsIndexer& indexer, const std::string& dev_path);
    ~TuiClient() = default;

    // Start the TUI interactive loop
    void run();

private:
    NtfsParser& parser_;
    NtfsIndexer& indexer_;
    std::string dev_path_;
};
