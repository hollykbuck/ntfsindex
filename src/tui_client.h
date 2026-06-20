#pragma once
#include "ntfs_indexer.h"
#include "ntfs_parser.h"

class TuiClient {
public:
    TuiClient(NtfsParser& parser, NtfsIndexer& indexer);
    ~TuiClient() = default;

    // Start the TUI interactive loop
    void run();

private:
    NtfsParser& parser_;
    NtfsIndexer& indexer_;
};
