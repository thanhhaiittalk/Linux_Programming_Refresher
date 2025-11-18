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

# -----------------------
# Build the Shared memory tool
# -----------------------
add_executable(shm_log
    Shm_app/shm_log.cpp
)
# Include directory
target_include_directories(log PRIVATE include)
target_include_directories(shm_log PRIVATE include)