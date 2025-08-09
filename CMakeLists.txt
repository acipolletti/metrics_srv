# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(MetricsServer VERSION 1.0.0 LANGUAGES CXX)

# Richiedi C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Opzioni di compilazione
option(BUILD_TESTS "Build test client" ON)
option(USE_SYSTEM_LIBS "Use system libraries instead of fetching" OFF)

# Thread support
find_package(Threads REQUIRED)

# OpenSSL
find_package(OpenSSL REQUIRED)

# zlib per compressione
find_package(ZLIB REQUIRED)

# Se non usiamo le librerie di sistema, scaricale
if(NOT USE_SYSTEM_LIBS)
    include(FetchContent)
    
    # cpp-httplib
    FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.14.3
    )
    FetchContent_MakeAvailable(httplib)
    
    # nlohmann/json
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(json)
else()
    # Usa pkg-config per trovare le librerie di sistema
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)
endif()

# Eseguibile server principale
add_executable(metrics_server metrics_server.cpp)

# Link delle librerie
target_link_libraries(metrics_server 
    PRIVATE 
        OpenSSL::SSL 
        OpenSSL::Crypto
        Threads::Threads
        ZLIB::ZLIB
)

if(NOT USE_SYSTEM_LIBS)
    target_link_libraries(metrics_server 
        PRIVATE 
            httplib::httplib
            nlohmann_json::nlohmann_json
    )
else()
    target_include_directories(metrics_server PRIVATE ${NLOHMANN_JSON_INCLUDE_DIRS})
    target_compile_options(metrics_server PRIVATE ${NLOHMANN_JSON_CFLAGS_OTHER})
endif()

# Definizioni per cpp-httplib
target_compile_definitions(metrics_server 
    PRIVATE 
        CPPHTTPLIB_OPENSSL_SUPPORT
        CPPHTTPLIB_ZLIB_SUPPORT
        CPPHTTPLIB_THREAD_POOL_COUNT=16
)

# Warning flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(metrics_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Client di test
if(BUILD_TESTS)
    add_executable(test_client test_client.cpp)
    
    target_link_libraries(test_client 
        PRIVATE 
            OpenSSL::SSL 
            OpenSSL::Crypto
            Threads::Threads
            ZLIB::ZLIB
    )
    
    if(NOT USE_SYSTEM_LIBS)
        target_link_libraries(test_client 
            PRIVATE 
                httplib::httplib
                nlohmann_json::nlohmann_json
        )
    else()
        target_include_directories(test_client PRIVATE ${NLOHMANN_JSON_INCLUDE_DIRS})
        target_compile_options(test_client PRIVATE ${NLOHMANN_JSON_CFLAGS_OTHER})
    endif()
    
    target_compile_definitions(test_client 
        PRIVATE 
            CPPHTTPLIB_OPENSSL_SUPPORT
            CPPHTTPLIB_ZLIB_SUPPORT
    )
endif()

# Script per generare certificati
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/generate_certs.sh
    ${CMAKE_CURRENT_BINARY_DIR}/generate_certs.sh
    COPYONLY
)

# Installa
install(TARGETS metrics_server DESTINATION bin)

# Crea script di avvio
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/start_server.sh
"#!/bin/bash
# Genera certificati se non esistono
if [ ! -f server.crt ]; then
    echo \"Generating certificates...\"
    ./generate_certs.sh
fi

# Avvia il server
./metrics_server --api-key \${API_KEY:-defaultkey} \$@
")

# Rendi eseguibile lo script
file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/start_server.sh
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)

# Messaggio di configurazione
message(STATUS "MetricsServer Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
message(STATUS "  Use System Libs: ${USE_SYSTEM_LIBS}")
message(STATUS "  OpenSSL Version: ${OPENSSL_VERSION}")