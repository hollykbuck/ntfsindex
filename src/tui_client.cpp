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

    // Helper function to update search results based on search_query
    auto update_search = [&]() {
        filtered_files.clear();
        file_names.clear();

        if (search_query.empty()) {
            // Show first 200 files if no search query
            for (size_t i = 0; i < std::min<size_t>(200, all_files.size()); ++i) {
                filtered_files.push_back(all_files[i]);
                std::string prefix = all_files[i]->is_directory ? "[DIR]  " : "[FILE] ";
                file_names.push_back(prefix + all_files[i]->full_path);
            }
        } else {
            std::string query_lower = search_query;
            std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

            for (const auto* file : all_files) {
                std::string path_lower = file->full_path;
                std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
                
                if (path_lower.find(query_lower) != std::string::npos) {
                    filtered_files.push_back(file);
                    std::string prefix = file->is_directory ? "[DIR]  " : "[FILE] ";
                    file_names.push_back(prefix + file->full_path);
                    
                    // Cap results to prevent TUI slowdowns
                    if (filtered_files.size() >= 500) {
                        break;
                    }
                }
            }
        }

        if (selected_index >= static_cast<int>(filtered_files.size())) {
            selected_index = std::max(0, static_cast<int>(filtered_files.size()) - 1);
        }
    };

    // Initial search population
    update_search();

    // 1. Search Box input component
    std::atomic<uint64_t> search_id{0};
    exec::single_thread_context search_ctx;
    auto search_scheduler = search_ctx.get_scheduler();
    exec::async_scope scope;

    InputOption input_opt;
    input_opt.on_change = [&]() {
        uint64_t my_id = ++search_id;
        std::string query_to_run = search_query;

        auto search_sender = stdexec::schedule(search_scheduler)
            | stdexec::then([&, my_id, query = std::move(query_to_run)]() {
                // Debounce delay
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                
                if (my_id != search_id.load()) {
                    return;
                }

                std::vector<const FileEntry*> temp_filtered;
                std::vector<std::string> temp_names;

                if (query.empty()) {
                    for (size_t i = 0; i < std::min<size_t>(200, all_files.size()); ++i) {
                        temp_filtered.push_back(all_files[i]);
                        std::string prefix = all_files[i]->is_directory ? "[DIR]  " : "[FILE] ";
                        temp_names.push_back(prefix + all_files[i]->full_path);
                    }
                } else {
                    std::string query_lower = query;
                    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

                    for (const auto* file : all_files) {
                        std::string path_lower = file->full_path;
                        std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);

                        if (path_lower.find(query_lower) != std::string::npos) {
                            temp_filtered.push_back(file);
                            std::string prefix = file->is_directory ? "[DIR]  " : "[FILE] ";
                            temp_names.push_back(prefix + file->full_path);

                            if (temp_filtered.size() >= 500) {
                                break;
                            }
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(search_mutex);
                    if (my_id == search_id.load()) {
                        bg_filtered_files = std::move(temp_filtered);
                        bg_file_names = std::move(temp_names);
                        bg_results_ready = true;
                        screen.PostEvent(Event::Custom);
                    }
                }
            });

        scope.spawn(std::move(search_sender));
    };
    Component input_field = Input(&search_query, "Type to search by name or path...", input_opt);

    // 2. Results list component
    Component file_list = Menu(&file_names, &selected_index);

    // Combine them in a vertical layout container
    auto container = Container::Vertical({
        input_field,
        file_list,
    });

    // Renderer for the UI layout
    auto renderer = Renderer(container, [&] {
        // Build the metadata/details panel
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

        return vbox({
            // Header bar
            hbox({
                text(" NTFS Indexer Terminal UI ") | bold | bgcolor(Color::Blue) | color(Color::White),
                filler(),
                text(" Use Up/Down to navigate list | Tab to switch fields | ESC to exit ") | dim
            }),
            separator(),
            
            // Search Input area
            hbox({
                text(" Search Path/File: ") | bold | color(Color::Yellow),
                input_field->Render() | flex,
            }),
            separator(),
            
            // Results list and details panel split
            hbox({
                window(text(fmt::format(" Search Results ({}) ", filtered_files.size())), 
                       file_list->Render() | vscroll_indicator | frame) | flex,
                window(text(" Properties "), vbox(std::move(details))) | size(WIDTH, EQUAL, 60)
            }) | flex
        });
    });

    // Add ESC key handler for exiting the application and background search events
    auto catch_exit = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }
        if (event == Event::Custom) {
            std::lock_guard<std::mutex> lock(search_mutex);
            if (bg_results_ready) {
                filtered_files = std::move(bg_filtered_files);
                file_names = std::move(bg_file_names);
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
