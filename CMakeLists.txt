cmake_minimum_required(VERSION 3.23)
project(ntfsindex LANGUAGES CXX VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies managed by Conan
find_package(fmt REQUIRED)

# Executable definition
add_executable(ntfsindex
    src/main.cpp
    src/ntfs_parser.cpp
    src/ntfs_parser.h
    src/ntfs_structs.h
)

# Link dependencies
target_link_libraries(ntfsindex PRIVATE fmt::fmt)

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
    src/ntfs_structs.h
)
target_link_libraries(ntfs_tests PRIVATE fmt::fmt Catch2::Catch2WithMain)
if(MSVC)
    target_compile_options(ntfs_tests PRIVATE /W4)
else()
    target_compile_options(ntfs_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME NtfsTests COMMAND ntfs_tests)
