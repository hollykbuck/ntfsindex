#define _CRT_SECURE_NO_WARNINGS
#include "tui_client.h"
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <algorithm>
#include <fmt/format.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <stdexec/execution.hpp>
#include <exec/single_thread_context.hpp>
#include <exec/async_scope.hpp>
#include <regex>
#include <fstream>
#include <sstream>
#include "absl/log/log.h"

using namespace ftxui;

namespace {

std::string format_bytes(uint64_t bytes) {
    if (bytes == 0) return "0 B";
    const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
    double s = static_cast<double>(bytes);
    int i = 0;
    while (s >= 1024.0 && i < 4) {
        s /= 1024.0;
        i++;
    }
    return fmt::format("{:.2f} {}", s, suffixes[i]);
}

std::string event_to_string(const Event& event) {
    if (event.is_character()) {
        std::string s = "Char(";
        for (char c : event.character()) {
            if (c < 32) s += fmt::format("\\x{:02X}", static_cast<int>(c));
            else s += c;
        }
        s += ")";
        return s;
    }
    if (event == Event::Custom) return "Custom";
    if (event == Event::Escape) return "Escape";
    if (event == Event::F1) return "F1";
    if (event == Event::F2) return "F2";
    if (event == Event::F3) return "F3";
    if (event == Event::F4) return "F4";
    if (event == Event::F5) return "F5";
    if (event == Event::F6) return "F6";
    if (event == Event::F7) return "F7";
    if (event == Event::F8) return "F8";
    if (event == Event::F9) return "F9";
    if (event == Event::F10) return "F10";
    return "Special/Unknown";
}

} // namespace

TuiClient::TuiClient(NtfsParser& parser, NtfsIndexer& indexer, const std::string& dev_path, const std::string& cache_file)
    : parser_(parser), indexer_(indexer), dev_path_(dev_path), cache_file_(cache_file) {}

void TuiClient::run() {
    LOG(INFO) << "TuiClient::run() entered.";
    auto screen = ScreenInteractive::Fullscreen();
    // screen.TrackMouse(false);

    std::string search_query = "";
    std::vector<const FileEntry*> filtered_files;
    std::vector<std::string> file_names;
    int selected_index = 0;

    std::mutex search_mutex;
    std::mutex auto_update_mutex;
    std::condition_variable auto_update_cv;
    std::atomic<bool> stop_auto_update{false};
    int auto_update_interval = 0;
    std::vector<std::string> auto_update_options = {"Off", "5s", "30s", "60s"};
    int auto_update_option_index = 0;
    
    std::vector<const FileEntry*> bg_filtered_files;
    std::vector<std::string> bg_file_names;
    std::string bg_regex_error = "";
    std::string regex_error = "";
    bool bg_results_ready = false;

    // Loading and query duration state
    bool is_searching = false;
    double search_duration_ms = 0.0;
    double bg_search_duration_ms = 0.0;

    // Cache pointers to all files in the index
    std::vector<const FileEntry*> all_files;

    // Scan phase and progress tracking
    enum class ScanPhase {
        Initializing,
        ParsingMFT,
        BuildingIndex,
        Completed,
        Failed
    };
    std::atomic<ScanPhase> scan_phase{ScanPhase::Initializing};
    std::atomic<uint64_t> scan_progress{0};
    std::atomic<uint64_t> scan_total{100};
    std::string scan_error_msg = "";

    // Log configuration and reading helpers
    std::string log_file_path = "out.log";
    const char* env_path = std::getenv("NTFSINDEX_LOG_FILE");
    if (env_path && env_path[0] != '\0') {
        log_file_path = env_path;
    }

    std::vector<std::string> log_lines;
    int selected_log_index = 0;

    auto read_log_file = [](const std::string& path, size_t max_lines = 200) -> std::vector<std::string> {
        std::ifstream file(path);
        if (!file.is_open()) {
            return {"Could not open log file: " + path};
        }
        
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        
        if (lines.size() > max_lines) {
            return std::vector<std::string>(lines.end() - max_lines, lines.end());
        }
        if (lines.empty()) {
            return {"(Log file is empty or has not been created yet)"};
        }
        return lines;
    };

    auto refresh_logs = [&]() {
        log_lines = read_log_file(log_file_path, 200);
        selected_log_index = std::max(0, static_cast<int>(log_lines.size()) - 1);
    };

    // Tab configurations
    int active_tab = 0;
    std::vector<std::string> tab_names = {"Search Files", "System Logs"};
    
    Component raw_tab_toggle = Toggle(&tab_names, &active_tab);
    Component tab_toggle = raw_tab_toggle | CatchEvent([raw_tab_toggle, &active_tab, &refresh_logs](Event event) {
        int old_val = active_tab;
        bool handled = raw_tab_toggle->OnEvent(event);
        if (active_tab != old_val) {
            if (active_tab == 1) {
                refresh_logs();
            }
        }
        return handled;
    });

    // Search panel controls
    bool use_regex = false;
    int type_filter = 0; // 0 = All, 1 = Files, 2 = Directories
    std::vector<std::string> type_filter_names = {"All", "Files", "Directories"};

    std::atomic<uint64_t> search_id{0};
    exec::single_thread_context search_ctx;
    auto search_scheduler = search_ctx.get_scheduler();
    exec::async_scope scope;

    auto trigger_search = [&]() {
        if (scan_phase.load() != ScanPhase::Completed) {
            return;
        }

        // Set searching state and trigger immediate screen refresh
        is_searching = true;
        screen.PostEvent(Event::Custom);

        uint64_t my_id = ++search_id;
        std::string query_to_run = search_query;
        int current_type_filter = type_filter;
        bool current_use_regex = use_regex;

        auto search_sender = stdexec::schedule(search_scheduler)
            | stdexec::then([&, my_id, query = std::move(query_to_run), current_type_filter, current_use_regex]() {
                try {
                    // Debounce delay
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    
                    if (my_id != search_id.load()) {
                        return;
                    }

                    auto start_time = std::chrono::high_resolution_clock::now();

                    std::vector<const FileEntry*> temp_filtered;
                    std::vector<std::string> temp_names;
                    bool regex_valid = true;
                    std::regex pattern;

                    if (current_use_regex && !query.empty()) {
                        try {
                            pattern = std::regex(query, std::regex_constants::ECMAScript | std::regex_constants::icase);
                        } catch (const std::regex_error&) {
                            regex_valid = false;
                        }
                    }

                    if (!regex_valid) {
                        {
                            std::lock_guard<std::mutex> lock(search_mutex);
                            if (my_id == search_id.load()) {
                                bg_filtered_files.clear();
                                bg_file_names.clear();
                                bg_regex_error = "Invalid Regular Expression";
                                bg_search_duration_ms = 0.0;
                                bg_results_ready = true;
                                screen.PostEvent(Event::Custom);
                            }
                        }
                        return;
                    }

                    std::string query_lower = query;
                    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

                    for (const auto* file : all_files) {
                        if (current_type_filter == 1 && file->is_directory) continue;
                        if (current_type_filter == 2 && !file->is_directory) continue;

                        bool match = false;
                        if (query.empty()) {
                            match = true;
                        } else if (current_use_regex) {
                            match = std::regex_search(file->name, pattern);
                        } else {
                            std::string name_lower = file->name;
                            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                            match = (name_lower.find(query_lower) != std::string::npos);
                        }

                        if (match) {
                            temp_filtered.push_back(file);
                            std::string full_path = indexer_.get_absolute_path(file->id);
                            std::string prefix = file->is_directory ? "[DIR]  " : "[FILE] ";
                            temp_names.push_back(prefix + full_path);

                            if (temp_filtered.size() >= 500) {
                                break;
                            }
                        }
                    }

                    auto end_time = std::chrono::high_resolution_clock::now();
                    double duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count() / 1000.0;

                    {
                        std::lock_guard<std::mutex> lock(search_mutex);
                        if (my_id == search_id.load()) {
                            bg_filtered_files = std::move(temp_filtered);
                            bg_file_names = std::move(temp_names);
                            bg_regex_error = "";
                            bg_search_duration_ms = duration;
                            bg_results_ready = true;
                            screen.PostEvent(Event::Custom);
                        }
                    }
                } catch (const std::exception& e) {
                    LOG(ERROR) << "Exception in search task: " << e.what();
                    std::lock_guard<std::mutex> lock(search_mutex);
                    if (my_id == search_id.load()) {
                        bg_filtered_files.clear();
                        bg_file_names.clear();
                        bg_regex_error = std::string("Error: ") + e.what();
                        bg_search_duration_ms = 0.0;
                        bg_results_ready = true;
                        screen.PostEvent(Event::Custom);
                    }
                } catch (...) {
                    LOG(ERROR) << "Unknown exception in search task.";
                    std::lock_guard<std::mutex> lock(search_mutex);
                    if (my_id == search_id.load()) {
                        bg_filtered_files.clear();
                        bg_file_names.clear();
                        bg_regex_error = "Unknown error during search.";
                        bg_search_duration_ms = 0.0;
                        bg_results_ready = true;
                        screen.PostEvent(Event::Custom);
                    }
                }
            })
            | stdexec::upon_error([](std::exception_ptr) {
                LOG(ERROR) << "Search task failed with unhandled sender error.";
            })
            | stdexec::upon_stopped([]() {
                LOG(INFO) << "Search task was stopped.";
            });

        scope.spawn(std::move(search_sender));
    };

    InputOption input_opt;
    input_opt.on_change = [&]() {
        trigger_search();
    };
    Component input_field = Input(&search_query, "Type to search by name or path...", input_opt);

    Component raw_regex_checkbox = Checkbox("Use Regex (F2)", &use_regex);
    Component regex_checkbox = raw_regex_checkbox | CatchEvent([raw_regex_checkbox, &use_regex, &trigger_search](Event event) {
        bool old_val = use_regex;
        bool handled = raw_regex_checkbox->OnEvent(event);
        if (use_regex != old_val) {
            trigger_search();
        }
        return handled;
    });

    Component raw_type_toggle = Toggle(&type_filter_names, &type_filter);
    Component type_toggle = raw_type_toggle | CatchEvent([raw_type_toggle, &type_filter, &trigger_search](Event event) {
        int old_val = type_filter;
        bool handled = raw_type_toggle->OnEvent(event);
        if (type_filter != old_val) {
            trigger_search();
        }
        return handled;
    });

    Component raw_auto_update_toggle = Toggle(&auto_update_options, &auto_update_option_index);
    Component auto_update_toggle = raw_auto_update_toggle | CatchEvent([raw_auto_update_toggle, &auto_update_option_index, &auto_update_interval, &auto_update_mutex, &auto_update_cv](Event event) {
        int old_val = auto_update_option_index;
        bool handled = raw_auto_update_toggle->OnEvent(event);
        if (auto_update_option_index != old_val) {
            int new_val = 0;
            if (auto_update_option_index == 1) new_val = 5;
            else if (auto_update_option_index == 2) new_val = 30;
            else if (auto_update_option_index == 3) new_val = 60;
            
            {
                std::lock_guard<std::mutex> lock(auto_update_mutex);
                auto_update_interval = new_val;
            }
            auto_update_cv.notify_all();
        }
        return handled;
    });

    Component file_list = Menu(&file_names, &selected_index);

    auto search_panel = Container::Vertical({
        input_field,
        Container::Horizontal({
            regex_checkbox,
            type_toggle,
            auto_update_toggle
        }),
        file_list
    });

    // System Logs panel controls
    Component log_list = Menu(&log_lines, &selected_log_index);
    Component refresh_btn = Button("Refresh Logs (F5)", refresh_logs);

    auto log_panel = Container::Vertical({
        refresh_btn,
        log_list
    });

    auto tab_container = Container::Tab({
        search_panel,
        log_panel
    }, &active_tab);

    auto main_container = Container::Vertical({
        tab_toggle,
        tab_container
    });

    auto renderer = Renderer(main_container, [&] {
        ScanPhase current_phase = scan_phase.load();

        if (current_phase != ScanPhase::Completed) {
            Elements body;
            
            if (current_phase == ScanPhase::Initializing) {
                body.push_back(text("Initializing NTFS partition reader...") | hcenter);
            } else if (current_phase == ScanPhase::ParsingMFT) {
                body.push_back(text("Parsing NTFS MFT metadata structures...") | hcenter);
            } else if (current_phase == ScanPhase::BuildingIndex) {
                float progress_val = static_cast<float>(scan_progress.load()) / static_cast<float>(std::max<uint64_t>(1, scan_total.load()));
                body.push_back(text(fmt::format("Scanning MFT records: {} / {}", scan_progress.load(), scan_total.load())) | hcenter);
                body.push_back(separator());
                body.push_back(gauge(progress_val) | color(Color::Blue));
                body.push_back(text(fmt::format("Progress: {:.1f}%", progress_val * 100.0f)) | hcenter);
            } else if (current_phase == ScanPhase::Failed) {
                body.push_back(text("MFT Scan Failed!") | bold | color(Color::Red) | hcenter);
                body.push_back(separator());
                body.push_back(text(scan_error_msg) | color(Color::Red) | hcenter);
                body.push_back(separator());
                body.push_back(text("Press Ctrl+Q to exit.") | dim | hcenter);
            }

            return vbox({
                hbox({
                    text(" NTFS Indexer Terminal UI ") | bold | bgcolor(Color::Blue) | color(Color::White),
                    filler(),
                    text(" Ctrl+Q to exit ") | dim
                }),
                separator(),
                filler(),
                window(text(" NTFS Partition Initializing and Indexing "), vbox(std::move(body))) 
                    | size(WIDTH, EQUAL, 80) | hcenter,
                filler()
            });
        }

        if (active_tab == 0) {
            Elements details;
            if (selected_index >= 0 && selected_index < static_cast<int>(filtered_files.size())) {
                const auto* file = filtered_files[selected_index];
                details.push_back(text("File Metadata Details") | bold | color(Color::Blue) | hcenter);
                details.push_back(separator());
                details.push_back(text(fmt::format("Name:       {}", file->name)) | bold | color(Color::Cyan));
                details.push_back(text(fmt::format("Full Path:  {}", indexer_.get_absolute_path(file->id))) | color(Color::White));
                details.push_back(text(fmt::format("Type:       {}", file->is_directory ? "Directory" : "File")));
                details.push_back(text(fmt::format("Size:       {} ({})", format_bytes(file->size), file->size)));
                details.push_back(text(fmt::format("MFT Record: {}", file->id)));
                details.push_back(text(fmt::format("Parent Record: {}", file->parent_id)));
            } else {
                details.push_back(text("No file selected or matched") | dim);
            }

            Element error_element = text("");
            if (!regex_error.empty()) {
                error_element = text(fmt::format(" [{}]", regex_error)) | bold | color(Color::Red);
            }

            std::string results_title;
            if (is_searching) {
                results_title = " Search Results (Loading...) ";
            } else if (search_query.empty()) {
                results_title = fmt::format(" Search Results ({} files) ", filtered_files.size());
            } else {
                results_title = fmt::format(" Search Results ({} matched in {:.2f} ms) ", filtered_files.size(), search_duration_ms);
            }

            Element results_window = window(text(results_title) | (is_searching ? color(Color::Yellow) : color(Color::Blue)),
                                            file_list->Render() | vscroll_indicator | frame) | flex;

            return vbox({
                // Header bar
                hbox({
                    text(" NTFS Indexer Terminal UI ") | bold | bgcolor(Color::Blue) | color(Color::White),
                    filler(),
                    text(" Use Tab to focus fields | Up/Down to navigate | Ctrl+Q to exit ") | dim
                }),
                separator(),
                
                // Tab Selection
                hbox({
                    text(" View: ") | bold | color(Color::Green),
                    tab_toggle->Render(),
                }),
                separator(),

                // Search Input
                hbox({
                    text(" Search: ") | bold | color(Color::Yellow),
                    input_field->Render() | flex,
                    error_element
                }),
                hbox({
                    regex_checkbox->Render(),
                    text(" | ") | dim,
                    text("Filter: ") | dim,
                    type_toggle->Render(),
                    text(" | ") | dim,
                    text("Auto Update: ") | bold | color(Color::Cyan),
                    auto_update_toggle->Render()
                }),
                separator(),
                
                // Results list and details panel split
                hbox({
                    results_window,
                    window(text(" Properties "), vbox(std::move(details))) | size(WIDTH, EQUAL, 60)
                }) | flex,
                
                // Footer
                separator(),
                hbox({
                    text(" F2/Ctrl+R: Regex | F3/Ctrl+F: Files | F4/Ctrl+D: Dirs | F5/Ctrl+A: All Filter | F6/Ctrl+L: Tab | Ctrl+Q: Exit ") | dim
                })
            });
        } else {
            return vbox({
                // Header bar
                hbox({
                    text(" NTFS Indexer Terminal UI ") | bold | bgcolor(Color::Blue) | color(Color::White),
                    filler(),
                    text(" Use Tab to focus fields | Up/Down to navigate | Ctrl+Q to exit ") | dim
                }),
                separator(),
                
                // Tab Selection
                hbox({
                    text(" View: ") | bold | color(Color::Green),
                    tab_toggle->Render(),
                }),
                separator(),

                // Log toolbar
                hbox({
                    text(fmt::format(" Log File: {} ", log_file_path)) | bold | color(Color::Yellow),
                    filler(),
                    refresh_btn->Render()
                }),
                separator(),

                // Log display
                window(text(" Log Entries (Last 200 Lines) "),
                       log_list->Render() | vscroll_indicator | frame) | flex,
                
                // Footer
                separator(),
                hbox({
                    text(" F5/Ctrl+G: Refresh Logs | F6/Ctrl+L: Tab | Ctrl+Q: Exit ") | dim
                })
            });
        }
    });

    auto is_ctrl_key = [](const Event& event, char c) {
        if (event.is_character() && event.character().size() == 1) {
            return event.character()[0] == (c - 'A' + 1);
        }
        return false;
    };

    auto catch_exit = CatchEvent(renderer, [&](Event event) {
        VLOG(1) << "TuiClient: CatchEvent received event: " << event_to_string(event);
        if (event == Event::F10 || is_ctrl_key(event, 'Q')) {
            LOG(INFO) << "TuiClient: Matched exit hotkey (F10 or Ctrl+Q). Triggering exit.";
            screen.ExitLoopClosure()();
            return true;
        }

        // If scanning is not complete, block all key inputs except custom completion notifications
        if (scan_phase.load() != ScanPhase::Completed) {
            if (event == Event::Custom) {
                std::lock_guard<std::mutex> lock(search_mutex);
                if (bg_results_ready) {
                    filtered_files = std::move(bg_filtered_files);
                    file_names = std::move(bg_file_names);
                    regex_error = std::move(bg_regex_error);
                    search_duration_ms = bg_search_duration_ms;
                    is_searching = false;
                    bg_results_ready = false;
                }
                return true;
            }
            return true;
        }

        // Tab Switch: F6 or Ctrl+L
        if (event == Event::F6 || is_ctrl_key(event, 'L')) {
            active_tab = 1 - active_tab;
            if (active_tab == 1) {
                refresh_logs();
            }
            return true;
        }

        // Search Tab shortcuts
        if (active_tab == 0) {
            if (event == Event::F2 || is_ctrl_key(event, 'R')) {
                use_regex = !use_regex;
                trigger_search();
                return true;
            }
            if (event == Event::F3 || is_ctrl_key(event, 'F')) {
                type_filter = 1;
                trigger_search();
                return true;
            }
            if (event == Event::F4 || is_ctrl_key(event, 'D')) {
                type_filter = 2;
                trigger_search();
                return true;
            }
            if (event == Event::F5 || is_ctrl_key(event, 'A')) {
                type_filter = 0;
                trigger_search();
                return true;
            }
        } else {
            // Logs Tab shortcuts
            if (event == Event::F5 || is_ctrl_key(event, 'G')) {
                refresh_logs();
                return true;
            }
        }

        if (event == Event::Custom) {
            std::lock_guard<std::mutex> lock(search_mutex);
            if (bg_results_ready) {
                filtered_files = std::move(bg_filtered_files);
                file_names = std::move(bg_file_names);
                regex_error = std::move(bg_regex_error);
                search_duration_ms = bg_search_duration_ms;
                is_searching = false;
                bg_results_ready = false;
                
                if (selected_index >= static_cast<int>(filtered_files.size())) {
                    selected_index = std::max(0, static_cast<int>(filtered_files.size()) - 1);
                }
            }
            return true;
        }
        return false;
    });

    // Start background scan task
    exec::single_thread_context scan_ctx;
    auto scan_scheduler = scan_ctx.get_scheduler();
    exec::async_scope scan_scope;

    auto scan_sender = stdexec::schedule(scan_scheduler)
        | stdexec::then([&]() {
            try {
                scan_phase = ScanPhase::ParsingMFT;
                screen.PostEvent(Event::Custom);

                LOG(INFO) << fmt::format("Initializing parser for device: {} ...", dev_path_);
                if (!parser_.init(dev_path_)) {
                    scan_phase = ScanPhase::Failed;
                    scan_error_msg = fmt::format("Failed to initialize device / block file: {}", dev_path_);
                    screen.PostEvent(Event::Custom);
                    return;
                }

                LOG(INFO) << "Starting MFT metadata parse...";
                if (!parser_.parse()) {
                    scan_phase = ScanPhase::Failed;
                scan_error_msg = "Failed to parse metadata and runs of $MFT.";
                    screen.PostEvent(Event::Custom);
                    return;
                }

                bool loaded_from_cache = false;
                if (!cache_file_.empty()) {
                    LOG(INFO) << fmt::format("Attempting to load index from cache file: {} ...", cache_file_);
                    if (indexer_.load_from_cache(cache_file_)) {
                        LOG(INFO) << "Successfully loaded index from cache. Applying pending USN Change Journal entries to catch up...";
                        if (!indexer_.update_index_incremental(parser_)) {
                            LOG(WARNING) << "Failed to perform incremental catch-up (USN journal may have been truncated or is disabled). Discarding cache and rebuilding index...";
                        } else {
                            LOG(INFO) << "Catch-up successful. Index is up to date.";
                            loaded_from_cache = true;
                        }
                    } else {
                        LOG(WARNING) << "Failed to load index from cache (or cache file does not exist). Proceeding with full scan.";
                    }
                }

                if (!loaded_from_cache) {
                    scan_phase = ScanPhase::BuildingIndex;
                    screen.PostEvent(Event::Custom);

                    LOG(INFO) << "Building initial file index...";
                    bool index_built = indexer_.build_initial_index(parser_, [&](uint64_t processed, uint64_t total) {
                        scan_progress = processed;
                        scan_total = total;
                        screen.PostEvent(Event::Custom);
                    });

                    if (!index_built) {
                        scan_phase = ScanPhase::Failed;
                        scan_error_msg = "Failed to scan MFT records or resolve parent paths.";
                        screen.PostEvent(Event::Custom);
                        return;
                    }

                    if (!cache_file_.empty()) {
                        LOG(INFO) << fmt::format("Saving built index to cache file: {} ...", cache_file_);
                        if (indexer_.save_to_cache(cache_file_)) {
                            LOG(INFO) << "Successfully saved index to cache.";
                        } else {
                            LOG(ERROR) << "Failed to save index to cache.";
                        }
                    }
                }

                // Populate all cached files on background thread before transitioning
                std::vector<const FileEntry*> temp_all_files;
                temp_all_files.reserve(indexer_.get_files().size());
                for (const auto& [id, entry] : indexer_.get_files()) {
                    temp_all_files.push_back(&entry);
                }
                std::sort(temp_all_files.begin(), temp_all_files.end(), [](const FileEntry* a, const FileEntry* b) {
                    if (a->is_directory != b->is_directory) {
                        return a->is_directory > b->is_directory;
                    }
                    return a->name < b->name;
                });

                {
                    std::lock_guard<std::mutex> lock(search_mutex);
                    all_files = std::move(temp_all_files);
                    
                    // Populate initial search view
                    bg_filtered_files.clear();
                    bg_file_names.clear();
                    for (size_t i = 0; i < std::min<size_t>(200, all_files.size()); ++i) {
                        bg_filtered_files.push_back(all_files[i]);
                        std::string prefix = all_files[i]->is_directory ? "[DIR]  " : "[FILE] ";
                        bg_file_names.push_back(prefix + indexer_.get_absolute_path(all_files[i]->id));
                    }
                    bg_results_ready = true;
                    scan_phase = ScanPhase::Completed;
                }
                screen.PostEvent(Event::Custom);
            } catch (const std::exception& e) {
                LOG(ERROR) << "Exception in background scan task: " << e.what();
                scan_phase = ScanPhase::Failed;
                scan_error_msg = fmt::format("Exception in scan task: {}", e.what());
                screen.PostEvent(Event::Custom);
            } catch (...) {
                LOG(ERROR) << "Unknown exception in background scan task.";
                scan_phase = ScanPhase::Failed;
                scan_error_msg = "Unknown exception in background scan task.";
                screen.PostEvent(Event::Custom);
            }
        })
        | stdexec::upon_error([&](std::exception_ptr) {
            LOG(ERROR) << "Background scan task failed with unhandled sender error.";
            scan_phase = ScanPhase::Failed;
            scan_error_msg = "Background scan task failed with unhandled sender error.";
            screen.PostEvent(Event::Custom);
        })
        | stdexec::upon_stopped([&]() {
            LOG(INFO) << "Background scan task was stopped.";
            scan_phase = ScanPhase::Failed;
            scan_error_msg = "Background scan task was stopped.";
            screen.PostEvent(Event::Custom);
        });

    std::thread auto_update_thread;
    auto_update_thread = std::thread([&]() {
        LOG(INFO) << "[TUI Auto Update] Background auto-update thread started.";
        while (!stop_auto_update.load()) {
            int interval = 0;
            {
                std::lock_guard<std::mutex> lock(auto_update_mutex);
                interval = auto_update_interval;
            }

            if (interval <= 0) {
                std::unique_lock<std::mutex> lock(auto_update_mutex);
                auto_update_cv.wait(lock, [&]() {
                    return stop_auto_update.load() || auto_update_interval > 0;
                });
                continue;
            }

            {
                std::unique_lock<std::mutex> lock(auto_update_mutex);
                if (auto_update_cv.wait_for(lock, std::chrono::seconds(interval), [&]() {
                    return stop_auto_update.load() || auto_update_interval != interval;
                })) {
                    // Woke up due to stop or interval change, re-evaluate
                    continue;
                }
            }

            // Dispatch update task to search scheduler to ensure serialization and safety
            auto update_sender = stdexec::schedule(search_scheduler)
                | stdexec::then([&]() {
                    LOG(INFO) << "[TUI Auto Update] Triggering scheduled incremental update...";
                    bool success = indexer_.update_index_incremental(parser_);
                    if (success) {
                        std::vector<const FileEntry*> temp_all_files;
                        temp_all_files.reserve(indexer_.get_files().size());
                        for (const auto& [id, entry] : indexer_.get_files()) {
                            temp_all_files.push_back(&entry);
                        }
                        std::sort(temp_all_files.begin(), temp_all_files.end(), [](const FileEntry* a, const FileEntry* b) {
                            if (a->is_directory != b->is_directory) {
                                return a->is_directory > b->is_directory;
                            }
                            return a->name < b->name;
                        });

                        {
                            std::lock_guard<std::mutex> lock(search_mutex);
                            all_files = std::move(temp_all_files);
                        }

                        // Re-trigger search to update view
                        trigger_search();
                    }
                });
            stdexec::sync_wait(std::move(update_sender));
        }
        LOG(INFO) << "[TUI Auto Update] Background auto-update thread stopped.";
    });

    LOG(INFO) << "TuiClient: Spawning scan_sender task.";
    scan_scope.spawn(std::move(scan_sender));

    LOG(INFO) << "TuiClient: Entering screen.Loop().";
    screen.Loop(catch_exit);
    LOG(INFO) << "TuiClient: screen.Loop() returned. Stopping auto-update thread...";

    {
        std::lock_guard<std::mutex> lock(auto_update_mutex);
        stop_auto_update = true;
    }
    auto_update_cv.notify_all();
    if (auto_update_thread.joinable()) {
        auto_update_thread.join();
    }

    stdexec::sync_wait(scope.on_empty());
    LOG(INFO) << "TuiClient: scope cleared.";
    stdexec::sync_wait(scan_scope.on_empty());
    LOG(INFO) << "TuiClient: scan_scope cleared. run() exiting.";
}
