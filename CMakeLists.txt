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

# -----------------------
# Build the CLI tool
# -----------------------
add_executable(logctl
    CLI_app/logctl.cpp
)

# Include directory
target_include_directories(log PRIVATE include)
