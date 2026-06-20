#pragma once

#include <string>
#include <mutex>

class NtfsParser;
class NtfsIndexer;

class HttpServer {
public:
    HttpServer(NtfsParser& parser, NtfsIndexer& indexer, const std::string& address, unsigned short port, const std::string& doc_root, const std::string& dev_path);
    ~HttpServer();

    // Start the server (blocks until stopped or error occurs)
    bool run();

    // Stop the server
    void stop();

private:
    NtfsParser& parser_;
    NtfsIndexer& indexer_;
    std::string address_;
    unsigned short port_;
    std::string doc_root_;
    std::string dev_path_;
    
    std::mutex mutex_; // Protects indexer/parser operations from concurrent HTTP request threads
};
