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
#include <stdexec/execution.hpp>
#include <exec/single_thread_context.hpp>
#include <exec/async_scope.hpp>
#include <regex>
#include <fstream>
#include <sstream>

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

} // namespace

TuiClient::TuiClient(NtfsParser& parser, NtfsIndexer& indexer)
    : parser_(parser), indexer_(indexer) {}

void TuiClient::run() {
    auto screen = ScreenInteractive::Fullscreen();

    std::string search_query = "";
    std::vector<const FileEntry*> filtered_files;
    std::vector<std::string> file_names;
    int selected_index = 0;

    std::mutex search_mutex;
    
    std::vector<const FileEntry*> bg_filtered_files;
    std::vector<std::string> bg_file_names;
    std::string bg_regex_error = "";
    std::string regex_error = "";
    bool bg_results_ready = false;

    // Cache pointers to all files in the index
    std::vector<const FileEntry*> all_files;
    all_files.reserve(indexer_.get_files().size());
    for (const auto& [id, entry] : indexer_.get_files()) {
        all_files.push_back(&entry);
    }

    // Sort files by path/name for a clean starting view
    std::sort(all_files.begin(), all_files.end(), [](const FileEntry* a, const FileEntry* b) {
        return a->full_path < b->full_path;
    });

    // Helper function to update search results (initial population)
    auto update_search = [&]() {
        filtered_files.clear();
        file_names.clear();

        for (const auto* file : all_files) {
            filtered_files.push_back(file);
            std::string prefix = file->is_directory ? "[DIR]  " : "[FILE] ";
            file_names.push_back(prefix + file->full_path);
            
            if (filtered_files.size() >= 200) {
                break;
            }
        }
        selected_index = 0;
    };

    // Initial search population
    update_search();

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
    
    Component tab_toggle = Toggle(&tab_names, &active_tab);
    tab_toggle = tab_toggle | CatchEvent([&](Event event) {
        int old_val = active_tab;
        bool handled = tab_toggle->OnEvent(event);
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
        uint64_t my_id = ++search_id;
        std::string query_to_run = search_query;
        int current_type_filter = type_filter;
        bool current_use_regex = use_regex;

        auto search_sender = stdexec::schedule(search_scheduler)
            | stdexec::then([&, my_id, query = std::move(query_to_run), current_type_filter, current_use_regex]() {
                // Debounce delay
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                
                if (my_id != search_id.load()) {
                    return;
                }

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
                        match = std::regex_search(file->full_path, pattern);
                    } else {
                        std::string path_lower = file->full_path;
                        std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
                        match = (path_lower.find(query_lower) != std::string::npos);
                    }

                    if (match) {
                        temp_filtered.push_back(file);
                        std::string prefix = file->is_directory ? "[DIR]  " : "[FILE] ";
                        temp_names.push_back(prefix + file->full_path);

                        if (temp_filtered.size() >= 500) {
                            break;
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(search_mutex);
                    if (my_id == search_id.load()) {
                        bg_filtered_files = std::move(temp_filtered);
                        bg_file_names = std::move(temp_names);
                        bg_regex_error = "";
                        bg_results_ready = true;
                        screen.PostEvent(Event::Custom);
                    }
                }
            });

        scope.spawn(std::move(search_sender));
    };

    InputOption input_opt;
    input_opt.on_change = [&]() {
        trigger_search();
    };
    Component input_field = Input(&search_query, "Type to search by name or path...", input_opt);

    Component regex_checkbox = Checkbox("Use Regex (F2)", &use_regex);
    regex_checkbox = regex_checkbox | CatchEvent([&](Event event) {
        bool old_val = use_regex;
        bool handled = regex_checkbox->OnEvent(event);
        if (use_regex != old_val) {
            trigger_search();
        }
        return handled;
    });

    Component type_toggle = Toggle(&type_filter_names, &type_filter);
    type_toggle = type_toggle | CatchEvent([&](Event event) {
        int old_val = type_filter;
        bool handled = type_toggle->OnEvent(event);
        if (type_filter != old_val) {
            trigger_search();
        }
        return handled;
    });

    Component file_list = Menu(&file_names, &selected_index);

    auto search_panel = Container::Vertical({
        input_field,
        Container::Horizontal({
            regex_checkbox,
            type_toggle
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
        if (active_tab == 0) {
            Elements details;
            if (selected_index >= 0 && selected_index < static_cast<int>(filtered_files.size())) {
                const auto* file = filtered_files[selected_index];
                details.push_back(text("File Metadata Details") | bold | color(Color::Blue) | hcenter);
                details.push_back(separator());
                details.push_back(text(fmt::format("Name:       {}", file->name)) | bold | color(Color::Cyan));
                details.push_back(text(fmt::format("Full Path:  {}", file->full_path)) | color(Color::White));
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

            return vbox({
                // Header bar
                hbox({
                    text(" NTFS Indexer Terminal UI ") | bold | bgcolor(Color::Blue) | color(Color::White),
                    filler(),
                    text(" Use Tab to focus fields | Up/Down to navigate | ESC to exit ") | dim
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
                    type_toggle->Render()
                }),
                separator(),
                
                // Results list and details panel split
                hbox({
                    window(text(fmt::format(" Search Results ({}) ", filtered_files.size())), 
                           file_list->Render() | vscroll_indicator | frame) | flex,
                    window(text(" Properties "), vbox(std::move(details))) | size(WIDTH, EQUAL, 60)
                }) | flex,
                
                // Footer
                separator(),
                hbox({
                    text(" F2/Ctrl+R: Regex | F3/Ctrl+F: Files | F4/Ctrl+D: Dirs | F5/Ctrl+A: All Filter | F6/Ctrl+L: Tab | ESC: Exit ") | dim
                })
            });
        } else {
            return vbox({
                // Header bar
                hbox({
                    text(" NTFS Indexer Terminal UI ") | bold | bgcolor(Color::Blue) | color(Color::White),
                    filler(),
                    text(" Use Tab to focus fields | Up/Down to navigate | ESC to exit ") | dim
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
                    text(" F5/Ctrl+G: Refresh Logs | F6/Ctrl+L: Tab | ESC: Exit ") | dim
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
        if (event == Event::Escape) {
            screen.ExitLoopClosure()();
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
                bg_results_ready = false;
                
                if (selected_index >= static_cast<int>(filtered_files.size())) {
                    selected_index = std::max(0, static_cast<int>(filtered_files.size()) - 1);
                }
            }
            return true;
        }
        return false;
    });

    screen.Loop(catch_exit);

    stdexec::sync_wait(scope.on_empty());
}
