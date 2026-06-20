cmake_minimum_required(VERSION 3.23)
project(ntfsindex LANGUAGES CXX VERSION 1.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# Find dependencies managed by Conan
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(absl REQUIRED)
find_package(ftxui REQUIRED)

# Executable definition
add_executable(ntfsindex
    src/main.cpp
    src/ntfs_parser.cpp
    src/ntfs_parser.h
    src/ntfs_indexer.cpp
    src/ntfs_indexer.h
    src/ntfs_structs.h
    src/http_server.cpp
    src/http_server.h
    src/tui_client.cpp
    src/tui_client.h
)

# Link dependencies
target_link_libraries(ntfsindex PRIVATE
    fmt::fmt
    boost::boost
    nlohmann_json::nlohmann_json
    absl::flags
    absl::flags_parse
    absl::log
    absl::log_initialize
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# Enable compiler warnings
if(MSVC)
    target_compile_options(ntfsindex PRIVATE /W4)
else()
    target_compile_options(ntfsindex PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Test configuration
find_package(Catch2 REQUIRED)
enable_testing()

add_executable(ntfs_tests
    tests/tests.cpp
    src/ntfs_parser.cpp
    src/ntfs_parser.h
    src/ntfs_indexer.cpp
    src/ntfs_indexer.h
    src/ntfs_structs.h
)
target_link_libraries(ntfs_tests PRIVATE
    fmt::fmt
    Catch2::Catch2WithMain
    absl::log
    absl::log_initialize
)
if(MSVC)
    target_compile_options(ntfs_tests PRIVATE /W4)
else()
    target_compile_options(ntfs_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME NtfsTests COMMAND ntfs_tests)
