#pragma once

#include <string>
#include <string_view>
#include <stdexec/execution.hpp>
#include <exec/single_thread_context.hpp>
#include <exec/asio/asio_thread_pool.hpp>
#include <exec/async_scope.hpp>
#include <utility>

class NtfsParser;
class NtfsIndexer;

class HttpServer {
public:
    using WorkerSchedulerType = decltype(std::declval<exec::single_thread_context>().get_scheduler());
    using IoSchedulerType = decltype(std::declval<exec::asio::asio_thread_pool>().get_scheduler());

    HttpServer(NtfsParser& parser, NtfsIndexer& indexer, WorkerSchedulerType worker_scheduler, IoSchedulerType io_scheduler, const std::string& address, unsigned short port, const std::string& doc_root, const std::string& dev_path);
    ~HttpServer();

    // Start the server (blocks until stopped or error occurs)
    bool run();

    // Stop the server
    void stop();

private:
    NtfsParser& parser_;
    NtfsIndexer& indexer_;
    WorkerSchedulerType worker_scheduler_;
    IoSchedulerType io_scheduler_;
    std::string address_;
    unsigned short port_;
    std::string doc_root_;
    std::string dev_path_;
    
    exec::async_scope scope_;
};

struct HttpContext {
    NtfsParser& parser;
    NtfsIndexer& indexer;
    HttpServer::WorkerSchedulerType worker_scheduler;
    HttpServer::IoSchedulerType io_scheduler;
    std::string_view doc_root;
    std::string_view dev_path;
};

namespace context {
    struct get_parser_t : stdexec::forwarding_query_t {
        template <typename Env>
        constexpr decltype(auto) operator()(const Env& env) const noexcept {
            return env.query(*this);
        }
    };
    inline constexpr get_parser_t get_parser{};

    struct get_indexer_t : stdexec::forwarding_query_t {
        template <typename Env>
        constexpr decltype(auto) operator()(const Env& env) const noexcept {
            return env.query(*this);
        }
    };
    inline constexpr get_indexer_t get_indexer{};

    struct get_worker_scheduler_t : stdexec::forwarding_query_t {
        template <typename Env>
        constexpr decltype(auto) operator()(const Env& env) const noexcept {
            return env.query(*this);
        }
    };
    inline constexpr get_worker_scheduler_t get_worker_scheduler{};

    struct get_io_scheduler_t : stdexec::forwarding_query_t {
        template <typename Env>
        constexpr decltype(auto) operator()(const Env& env) const noexcept {
            return env.query(*this);
        }
    };
    inline constexpr get_io_scheduler_t get_io_scheduler{};

    struct get_doc_root_t : stdexec::forwarding_query_t {
        template <typename Env>
        constexpr decltype(auto) operator()(const Env& env) const noexcept {
            return env.query(*this);
        }
    };
    inline constexpr get_doc_root_t get_doc_root{};

    struct get_dev_path_t : stdexec::forwarding_query_t {
        template <typename Env>
        constexpr decltype(auto) operator()(const Env& env) const noexcept {
            return env.query(*this);
        }
    };
    inline constexpr get_dev_path_t get_dev_path{};
}

