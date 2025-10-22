cmake_minimum_required(VERSION 3.12)

project(log LANGUAGES CXX)

# Use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
add_executable(log
    src/main.cpp
    src/cli.cpp
    src/app.cpp
)

# Include directory
target_include_directories(log PRIVATE include)
