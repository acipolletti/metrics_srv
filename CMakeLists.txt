cmake_minimum_required(VERSION 3.16)
project(metrics_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build options
option(ENABLE_PARQUET "Enable Parquet storage backend" ON)
option(ENABLE_REDIS "Enable Redis TimeSeries backend" ON)
option(BUILD_OPTIMIZED "Enable aggressive optimizations" ON)

# FetchContent for dependencies
include(FetchContent)

# cpp-httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.3
)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(httplib json)

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Arrow and Parquet (if enabled)
if(ENABLE_PARQUET)
    find_package(Arrow REQUIRED)
    find_package(Parquet REQUIRED)
    
    if(NOT Arrow_FOUND OR NOT Parquet_FOUND)
        message(STATUS "Arrow/Parquet not found via find_package, trying FetchContent...")
        
        FetchContent_Declare(
            arrow
            GIT_REPOSITORY https://github.com/apache/arrow.git
            GIT_TAG apache-arrow-14.0.0
            GIT_SHALLOW TRUE
        )
        
        set(ARROW_BUILD_STATIC ON)
        set(ARROW_BUILD_SHARED OFF)
        set(ARROW_PARQUET ON)
        set(ARROW_WITH_SNAPPY ON)
        set(ARROW_WITH_ZLIB ON)
        set(ARROW_WITH_LZ4 ON)
        set(ARROW_COMPUTE ON)
        set(ARROW_FILESYSTEM ON)
        
        FetchContent_MakeAvailable(arrow)
    endif()
endif()

# Main executable
add_executable(metrics_server 
    metrics_server.cpp
)

# Base libraries
target_link_libraries(metrics_server 
    PRIVATE 
        httplib::httplib
        nlohmann_json::nlohmann_json
        OpenSSL::SSL 
        OpenSSL::Crypto
        Threads::Threads
)

# Add Parquet support if enabled
if(ENABLE_PARQUET)
    target_compile_definitions(metrics_server PRIVATE ENABLE_PARQUET_STORAGE)
    
    if(TARGET Arrow::arrow_shared)
        target_link_libraries(metrics_server PRIVATE 
            Arrow::arrow_shared
            Parquet::parquet_shared
        )
    else()
        target_link_libraries(metrics_server PRIVATE 
            arrow_static
            parquet_static
        )
    endif()
    
    message(STATUS "Parquet storage backend: ENABLED")
else()
    message(STATUS "Parquet storage backend: DISABLED")
endif()

# Include directories
target_include_directories(metrics_server 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Enable HTTPS support
target_compile_definitions(metrics_server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# Optimization flags
if(BUILD_OPTIMIZED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(metrics_server PRIVATE 
        -O3
        -march=native
        -mtune=native
        -flto
        -funroll-loops
        -ffast-math
        -fprefetch-loop-arrays
        -ftree-vectorize
        -pthread
    )
    
    target_link_options(metrics_server PRIVATE
        -flto
        -O3
        -pthread
        -Wl,--gc-sections
    )
    
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    
    message(STATUS "Optimizations: ENABLED (O3, native arch, LTO)")
endif()

# Debug flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(metrics_server PRIVATE 
        -g
        -O0
        -Wall
        -Wextra
        -fsanitize=address
        -fsanitize=undefined
    )
    
    target_link_options(metrics_server PRIVATE
        -fsanitize=address
        -fsanitize=undefined
    )
endif()

# Installation instructions for Arrow/Parquet
message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Parquet storage: ${ENABLE_PARQUET}")
message(STATUS "Redis backend: ${ENABLE_REDIS}")
message(STATUS "Optimizations: ${BUILD_OPTIMIZED}")
message(STATUS "")

if(ENABLE_PARQUET AND NOT Arrow_FOUND)
    message(STATUS "To install Arrow and Parquet on Ubuntu/Debian:")
    message(STATUS "  sudo apt install libarrow-dev libparquet-dev")
    message(STATUS "")
    message(STATUS "Or with conda:")
    message(STATUS "  conda install arrow-cpp parquet-cpp -c conda-forge")
    message(STATUS "")
endif()

# Create Python requirements file for analytics
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/requirements.txt 
"# Python requirements for Parquet analytics
duckdb>=0.9.0
pandas>=2.0.0
pyarrow>=14.0.0
matplotlib>=3.5.0
seaborn>=0.12.0
jupyterlab>=4.0.0
")

message(STATUS "Python analytics requirements saved to: ${CMAKE_CURRENT_BINARY_DIR}/requirements.txt")
message(STATUS "Install with: pip install -r requirements.txt")