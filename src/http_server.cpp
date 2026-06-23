#include "http_server.h"
#include "ntfs_parser.h"
#include "absl/log/log.h"
#include "ntfs_indexer.h"
#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>
#include <exec/asio/use_sender.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/config.hpp>
#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
using json = nlohmann::json;

namespace {

// Return a reasonable mime type based on the file extension
beast::string_view mime_type(beast::string_view path) {
    using beast::iequals;
    auto const ext = [&path]() -> beast::string_view {
        auto const pos = path.rfind(".");
        if(pos == beast::string_view::npos)
            return "";
        return path.substr(pos);
    }();
    if(iequals(ext, ".html")) return "text/html";
    if(iequals(ext, ".htm"))  return "text/html";
    if(iequals(ext, ".css"))  return "text/css";
    if(iequals(ext, ".txt"))  return "text/plain";
    if(iequals(ext, ".js"))   return "application/javascript";
    if(iequals(ext, ".json")) return "application/json";
    if(iequals(ext, ".xml"))  return "application/xml";
    if(iequals(ext, ".png"))  return "image/png";
    if(iequals(ext, ".jpe"))  return "image/jpeg";
    if(iequals(ext, ".jpeg")) return "image/jpeg";
    if(iequals(ext, ".jpg"))  return "image/jpeg";
    if(iequals(ext, ".gif"))  return "image/gif";
    if(iequals(ext, ".bmp"))  return "image/bmp";
    if(iequals(ext, ".ico"))  return "image/x-icon";
    if(iequals(ext, ".tiff")) return "image/tiff";
    if(iequals(ext, ".tif"))  return "image/tiff";
    if(iequals(ext, ".svg"))  return "image/svg+xml";
    if(iequals(ext, ".svgz")) return "image/svg+xml";
    return "application/octet-stream";
}

// Append an HTTP rel-path to a local filesystem path
std::string path_cat(beast::string_view base, beast::string_view path) {
    if(base.empty())
        return std::string(path);
    std::string result(base);
#ifdef _WIN32
    char constexpr path_separator = '\\';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
    for(auto& c : result)
        if(c == '/')
            c = path_separator;
#else
    char constexpr path_separator = '/';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
#endif
    return result;
}

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%') {
            if (i + 2 < in.size()) {
                int value = 0;
                std::istringstream is(in.substr(i + 1, 2));
                if (is >> std::hex >> value) {
                    out += static_cast<char>(value);
                    i += 2;
                } else {
                    out += '%';
                }
            } else {
                out += '%';
            }
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query_params(const std::string& target) {
    std::unordered_map<std::string, std::string> params;
    auto query_pos = target.find('?');
    if (query_pos == std::string::npos) {
        return params;
    }
    std::string query_str = target.substr(query_pos + 1);
    std::istringstream ss(query_str);
    std::string item;
    while (std::getline(ss, item, '&')) {
        auto eq_pos = item.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = item.substr(0, eq_pos);
            std::string value = item.substr(eq_pos + 1);
            params[url_decode(key)] = url_decode(value);
        } else {
            params[url_decode(item)] = "";
        }
    }
    return params;
}

// Convert a std::string to lowercase
std::string to_lowercase(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return lower;
}

// Produce an HTTP response for the request
template<class Body, class Allocator>
http::message_generator
handle_request(
    http::request<Body, http::basic_fields<Allocator>>&& req,
    const HttpContext& ctx)
{
    auto& parser = ctx.parser;
    auto& indexer = ctx.indexer;
    auto scheduler = ctx.worker_scheduler;
    beast::string_view doc_root = ctx.doc_root;
    std::string dev_path(ctx.dev_path);

    auto const bad_request =
    [&req](beast::string_view why)
    {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = std::string(why);
        res.prepare_payload();
        return res;
    };

    auto const not_found =
    [&req](beast::string_view target)
    {
        http::response<http::string_body> res{http::status::not_found, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = "The resource '" + std::string(target) + "' was not found.";
        res.prepare_payload();
        return res;
    };

    auto const server_error =
    [&req](beast::string_view what)
    {
        http::response<http::string_body> res{http::status::internal_server_error, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = "An error occurred: '" + std::string(what) + "'";
        res.prepare_payload();
        return res;
    };

    auto const json_response =
    [&req](const json& j)
    {
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "application/json");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = j.dump();
        res.prepare_payload();
        return res;
    };

    // Handle OPTIONS (CORS preflight)
    if(req.method() == http::verb::options) {
        http::response<http::empty_body> res{http::status::no_content, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::access_control_allow_origin, "*");
        res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
        res.set(http::field::access_control_allow_headers, "Content-Type");
        res.keep_alive(req.keep_alive());
        return res;
    }

    std::string target_str = std::string(req.target());
    std::string path_only = target_str;
    auto query_pos = path_only.find('?');
    if (query_pos != std::string::npos) {
        path_only = path_only.substr(0, query_pos);
    }

    // API Routing
    if (path_only == "/api/search") {
        if (req.method() != http::verb::get) {
            return bad_request("Unknown HTTP-method for /api/search");
        }
        auto params = parse_query_params(target_str);
        std::string query = params["q"];
        
        size_t limit = 100;
        if (params.find("limit") != params.end()) {
            try {
                limit = std::stoul(params["limit"]);
            } catch (...) {}
        }

        std::string query_lower = to_lowercase(query);

        auto search_sender = stdexec::schedule(scheduler)
            | stdexec::then([&]() -> std::vector<FileEntry> {
                std::vector<FileEntry> m;
                const auto& files = indexer.get_files();
                for (const auto& [id, entry] : files) {
                    std::string name_lower = to_lowercase(entry.name);
                    std::string path_lower = to_lowercase(entry.full_path);

                    if (name_lower.find(query_lower) != std::string::npos || path_lower.find(query_lower) != std::string::npos) {
                        m.push_back(entry);
                    }
                }
                return m;
            });

        auto search_result = stdexec::sync_wait(std::move(search_sender));
        std::vector<FileEntry> matches = search_result ? std::get<0>(*search_result) : std::vector<FileEntry>{};


        // Sort matches: Directories first, then alphabetically by full path
        std::sort(matches.begin(), matches.end(), [](const FileEntry& a, const FileEntry& b) {
            if (a.is_directory != b.is_directory) {
                return a.is_directory > b.is_directory; // true (1) before false (0)
            }
            return a.full_path < b.full_path;
        });

        // Limit results
        json results = json::array();
        size_t display_count = std::min(matches.size(), limit);
        for (size_t i = 0; i < display_count; ++i) {
            const auto& entry = matches[i];
            results.push_back({
                {"id", entry.id},
                {"parent_id", entry.parent_id},
                {"name", entry.name},
                {"is_directory", entry.is_directory},
                {"size", entry.size},
                {"full_path", entry.full_path}
            });
        }

        json response_json = {
            {"total_matches", matches.size()},
            {"limit", limit},
            {"results", results}
        };

        return json_response(response_json);
    }
    
    if (path_only == "/api/stats") {
        if (req.method() != http::verb::get) {
            return bad_request("Unknown HTTP-method for /api/stats");
        }

        struct StatsResult {
            size_t file_count = 0;
            size_t dir_count = 0;
            uint64_t total_bytes = 0;
        };

        auto stats_sender = stdexec::schedule(scheduler)
            | stdexec::then([&]() -> StatsResult {
                StatsResult res;
                const auto& files = indexer.get_files();
                for (const auto& [id, entry] : files) {
                    if (entry.is_directory) {
                        res.dir_count++;
                    } else {
                        res.file_count++;
                        res.total_bytes += entry.size;
                    }
                }
                return res;
            });

        auto stats_result = stdexec::sync_wait(std::move(stats_sender));
        size_t file_count = 0;
        size_t dir_count = 0;
        uint64_t total_bytes = 0;
        if (stats_result) {
            auto res = std::get<0>(*stats_result);
            file_count = res.file_count;
            dir_count = res.dir_count;
            total_bytes = res.total_bytes;
        }


        auto format_size_local = [](uint64_t bytes) {
            const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
            int suffix_idx = 0;
            double size = static_cast<double>(bytes);
            while (size >= 1024.0 && suffix_idx < 4) {
                size /= 1024.0;
                suffix_idx++;
            }
            return fmt::format("{:.2f} {}", size, suffixes[suffix_idx]);
        };

        json stats = {
            {"device_path", dev_path},
            {"total_directories", dir_count},
            {"total_files", file_count},
            {"total_logical_size", total_bytes},
            {"total_logical_size_formatted", format_size_local(total_bytes)}
        };

        return json_response(stats);
    }

    if (path_only == "/api/usn") {
        if (req.method() != http::verb::get) {
            return bad_request("Unknown HTTP-method for /api/usn");
        }

        auto params = parse_query_params(target_str);
        size_t limit = 100;
        if (params.find("limit") != params.end()) {
            try {
                limit = std::stoul(params["limit"]);
            } catch (...) {}
        }

        struct UsnResult {
            bool success = false;
            std::vector<NtfsParser::UsnJournalEntry> usn_entries;
        };

        auto usn_sender = stdexec::schedule(scheduler)
            | stdexec::then([&]() -> UsnResult {
                UsnResult res;
                const auto& files = indexer.get_files();

                uint64_t usn_mft_idx = 0;
                for (const auto& [id, entry] : files) {
                    if (entry.parent_id == 11 && entry.name == "$UsnJrnl") {
                        usn_mft_idx = id;
                        break;
                    }
                }

                if (usn_mft_idx != 0) {
                    res.success = parser.parse_usn_journal(res.usn_entries, usn_mft_idx);
                }
                return res;
            });

        auto usn_result = stdexec::sync_wait(std::move(usn_sender));
        std::vector<NtfsParser::UsnJournalEntry> usn_entries;
        bool success = false;
        if (usn_result) {
            auto res = std::get<0>(*usn_result);
            success = res.success;
            usn_entries = std::move(res.usn_entries);
        }


        if (!success) {
            return json_response(json{
                {"success", false},
                {"message", "USN Change Journal ($UsnJrnl) is not active or could not be parsed on this volume."}
            });
        }

        auto format_filetime_local = [](uint64_t filetime) {
            if (filetime == 0) return std::string("-");
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
        };

        auto format_usn_reason_local = [](uint32_t reason) {
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
        };

        json list = json::array();
        size_t start_idx = usn_entries.size() > limit ? usn_entries.size() - limit : 0;
        for (size_t i = start_idx; i < usn_entries.size(); ++i) {
            const auto& entry = usn_entries[i];
            list.push_back({
                {"usn", fmt::format("0x{:016X}", entry.usn)},
                {"file_id", entry.file_id},
                {"parent_id", entry.parent_id},
                {"filename", entry.filename},
                {"reason", format_usn_reason_local(entry.reason)},
                {"timestamp", format_filetime_local(entry.timestamp)}
            });
        }

        return json_response(json{
            {"success", true},
            {"total_records", usn_entries.size()},
            {"entries", list}
        });
    }

    if (path_only == "/api/update") {
        if (req.method() != http::verb::post) {
            return bad_request("Unknown HTTP-method for /api/update");
        }

        auto start_time = std::chrono::high_resolution_clock::now();

        auto update_sender = stdexec::schedule(scheduler)
            | stdexec::then([&]() -> bool {
                return indexer.update_index_incremental(parser);
            });

        auto update_result = stdexec::sync_wait(std::move(update_sender));
        bool success = update_result ? std::get<0>(*update_result) : false;


        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        if (!success) {
            return json_response(json{
                {"success", false},
                {"message", "Incremental index update failed."}
            });
        }

        return json_response(json{
            {"success", true},
            {"elapsed_ms", elapsed.count()},
            {"last_usn", fmt::format("0x{:X}", indexer.get_last_usn())}
        });
    }

    // Serve static files
    if(req.method() != http::verb::get && req.method() != http::verb::head) {
        return bad_request("Unknown HTTP-method for static files");
    }

    if(path_only.empty() || path_only[0] != '/' || path_only.find("..") != std::string::npos) {
        return bad_request("Illegal request-target");
    }

    std::string full_path = path_cat(doc_root, path_only);
    if(path_only.back() == '/') {
        full_path = path_cat(doc_root, path_only + "index.html");
    }

    beast::error_code ec;
    http::file_body::value_type body;
    body.open(full_path.c_str(), beast::file_mode::scan, ec);

    if(ec == beast::errc::no_such_file_or_directory) {
        return not_found(req.target());
    }

    if(ec) {
        return server_error(ec.message());
    }

    auto const size = body.size();

    if(req.method() == http::verb::head) {
        http::response<http::empty_body> res{http::status::ok, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, mime_type(full_path));
        res.content_length(size);
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        return res;
    }

    http::response<http::file_body> res{
        std::piecewise_construct,
        std::make_tuple(std::move(body)),
        std::make_tuple(http::status::ok, req.version())};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, mime_type(full_path));
    res.content_length(size);
    res.set(http::field::access_control_allow_origin, "*");
    res.keep_alive(req.keep_alive());
    return res;
}

inline auto read_context() {
    return stdexec::read_env(context::get_context);
}

// Handles an HTTP server connection
void do_session(tcp::socket socket, const HttpContext& ctx, exec::async_scope& scope) {
    struct Session {
        tcp::socket socket;
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
    };
    
    auto s = std::make_shared<Session>(Session{std::move(socket), {}, {}});
    
    auto run_cycle = std::make_shared<std::function<void()>>();
    *run_cycle = [s, ctx, &scope, run_cycle]() {
        auto read_sender = http::async_read(s->socket, s->buffer, s->req, exec::asio::use_sender);
        
        auto pipeline = std::move(read_sender)
            | stdexec::continues_on(ctx.worker_scheduler)
            | stdexec::let_value([s](std::size_t) mutable {
                return read_context()
                    | stdexec::then([s](const HttpContext& read_ctx) mutable {
                        auto msg = handle_request(std::move(s->req), read_ctx);
                        bool keep_alive = msg.keep_alive();
                        return std::make_pair(std::move(msg), keep_alive);
                    });
            })
            | stdexec::continues_on(ctx.io_scheduler)
            | stdexec::let_value([s](std::pair<http::message_generator, bool>& result) mutable {
                auto msg = std::move(result.first);
                bool keep_alive = result.second;
                return beast::async_write(s->socket, std::move(msg), exec::asio::use_sender)
                    | stdexec::then([keep_alive](std::size_t) {
                        return keep_alive;
                    });
            })
            | stdexec::then([s, run_cycle](bool keep_alive) {
                if (keep_alive) {
                    s->req = {};
                    (*run_cycle)();
                } else {
                    boost::system::error_code ec;
                    s->socket.shutdown(tcp::socket::shutdown_both, ec);
                }
            })
            | stdexec::upon_error([s](std::exception_ptr) {
                boost::system::error_code ec;
                s->socket.shutdown(tcp::socket::shutdown_both, ec);
            })
            | stdexec::upon_stopped([s]() {
                boost::system::error_code ec;
                s->socket.shutdown(tcp::socket::shutdown_both, ec);
            })
            | stdexec::write_env(stdexec::prop{context::get_context, std::ref(ctx)});

        scope.spawn(std::move(pipeline));
    };

    (*run_cycle)();
}

} // namespace

HttpServer::HttpServer(NtfsParser& parser, NtfsIndexer& indexer, WorkerSchedulerType worker_scheduler, IoSchedulerType io_scheduler, const std::string& address, unsigned short port, const std::string& doc_root, const std::string& dev_path)
    : parser_(parser), indexer_(indexer), worker_scheduler_(worker_scheduler), io_scheduler_(io_scheduler), address_(address), port_(port), doc_root_(doc_root), dev_path_(dev_path) {}

HttpServer::~HttpServer() {
    stdexec::sync_wait(scope_.on_empty());
}

bool HttpServer::run() {
    try {
        net::io_context ioc{1};
        tcp::acceptor acceptor{ioc, {net::ip::make_address(address_), port_}};
        LOG(INFO) << fmt::format("[HTTP Server] Listening on http://{}:{}/", address_, port_);
        LOG(INFO) << fmt::format("[HTTP Server] Serving static files from: {}", doc_root_);

        HttpContext ctx{
            parser_,
            indexer_,
            worker_scheduler_,
            io_scheduler_,
            doc_root_,
            dev_path_
        };

        for(;;) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);

            auto session_sender = stdexec::schedule(io_scheduler_)
                | stdexec::then([s = std::move(socket), this, ctx]() mutable {
                    do_session(
                        std::move(s),
                        ctx,
                        scope_
                    );
                });
            scope_.spawn(std::move(session_sender));
        }
    } catch (const std::exception& e) {
        LOG(ERROR) << "[HTTP Server] Error: " << e.what();
        return false;
    }
    return true;
}

void HttpServer::stop() {
    stdexec::sync_wait(scope_.on_empty());
}

