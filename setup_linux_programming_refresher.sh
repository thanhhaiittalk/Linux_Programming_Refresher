#!/usr/bin/env bash
set -euo pipefail

PROJ="log"
echo "Creating project scaffold: $PROJ ..."

# Create directories
mkdir -p ./{src,include,build,logs}

# Create empty files
touch ./src/main.cpp
touch ./include/${PROJ}.hpp

# Create CMakeLists.txt
cat > ./CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.12)
project(log LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(log
    src/main.cpp
)

target_include_directories(log PRIVATE include)
EOF

# Create .gitignore
cat > ./.gitignore <<'EOF'
/build/
/logs/
/cmake-build-*/
*.o
*.log
.DS_Store
EOF

# Create README.md
cat > ./README.md <<'EOF'
# Day 1 – Project Scaffold & Basic Logging

## Structure
