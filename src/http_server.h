#pragma once

#include <string>
#include <stdexec/execution.hpp>
#include <exec/single_thread_context.hpp>
#include <utility>

class NtfsParser;
class NtfsIndexer;

class HttpServer {
public:
    using SchedulerType = decltype(std::declval<exec::single_thread_context>().get_scheduler());

    HttpServer(NtfsParser& parser, NtfsIndexer& indexer, SchedulerType scheduler, const std::string& address, unsigned short port, const std::string& doc_root, const std::string& dev_path);
    ~HttpServer();

    // Start the server (blocks until stopped or error occurs)
    bool run();

    // Stop the server
    void stop();

private:
    NtfsParser& parser_;
    NtfsIndexer& indexer_;
    SchedulerType scheduler_;
    std::string address_;
    unsigned short port_;
    std::string doc_root_;
    std::string dev_path_;
};

