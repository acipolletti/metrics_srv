cmake_minimum_required(VERSION 3.14)
project(metrics_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Abilita interprocedural optimization (LTO) per Release
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# FetchContent per scaricare le dipendenze
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

# Trova OpenSSL
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Eseguibile principale
add_executable(metrics_server metrics_server.cpp)

# Link delle librerie
target_link_libraries(metrics_server 
    PRIVATE 
        httplib::httplib
        nlohmann_json::nlohmann_json
        OpenSSL::SSL 
        OpenSSL::Crypto
        Threads::Threads
)

# Include directories
target_include_directories(metrics_server 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Definisci CPPHTTPLIB_OPENSSL_SUPPORT per abilitare HTTPS
target_compile_definitions(metrics_server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# ============================================================================
# OTTIMIZZAZIONI DI COMPILAZIONE
# ============================================================================

# Opzioni di compilazione per Release (MASSIME PRESTAZIONI)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # Ottimizzazioni aggressive
    target_compile_options(metrics_server PRIVATE 
        -O3                      # Massimo livello di ottimizzazione
        -march=native            # Ottimizza per l'architettura CPU corrente
        -mtune=native            # Fine-tuning per la CPU corrente
        -flto                    # Link Time Optimization
        -funroll-loops           # Srotola i loop per performance
        -ffast-math              # Ottimizzazioni matematiche aggressive
        -fprefetch-loop-arrays   # Prefetch degli array nei loop
        -ftree-vectorize         # Auto-vectorization
        -pthread                 # Supporto threading ottimizzato
    )
    
    # Ottimizzazioni specifiche per GCC
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(metrics_server PRIVATE
            -fipa-pta               # Interprocedural pointer analysis
            -fdevirtualize-at-ltrans # Devirtualizzazione aggressiva
            -floop-parallelize-all  # Parallelizzazione automatica loop
            -ftree-parallelize-loops=4 # Usa 4 thread per parallelizzazione
        )
    endif()
    
    # Link options per performance
    target_link_options(metrics_server PRIVATE
        -flto                    # Link Time Optimization
        -O3                      # Ottimizzazione anche nel linking
        -pthread                 # Threading ottimizzato
        -Wl,--gc-sections        # Rimuovi sezioni non usate
    )
    
    # Definizioni per Release
    target_compile_definitions(metrics_server PRIVATE
        NDEBUG                   # Disabilita assert
        BOOST_DISABLE_ASSERTS    # Disabilita assert di Boost se usato
    )
endif()

# Opzioni di compilazione per Debug
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(metrics_server PRIVATE 
        -g                       # Simboli di debug
        -O0                      # Nessuna ottimizzazione
        -Wall                    # Tutti i warning
        -Wextra                  # Warning extra
        -Wpedantic               # Aderenza stretta allo standard
        -Wformat=2               # Controlli format string
        -Wnull-dereference       # Controllo null pointer
        -Wdouble-promotion       # Warning su promozioni float->double
        -Wshadow                 # Warning su variable shadowing
        -fstack-protector-strong # Protezione stack
        -fsanitize=address       # AddressSanitizer per debug memoria
        -fsanitize=undefined     # UndefinedBehaviorSanitizer
    )
    
    target_link_options(metrics_server PRIVATE
        -fsanitize=address
        -fsanitize=undefined
    )
    
    target_compile_definitions(metrics_server PRIVATE
        DEBUG
        _DEBUG
    )
endif()

# Build type Profile (per profiling con simboli ma ottimizzato)
if(CMAKE_BUILD_TYPE STREQUAL "Profile")
    target_compile_options(metrics_server PRIVATE
        -O3
        -g
        -fno-omit-frame-pointer  # Mantieni frame pointer per profiling
        -march=native
        -mtune=native
    )
endif()

# ============================================================================
# CONFIGURAZIONI ADDIZIONALI
# ============================================================================

# Abilita color output per compilatori che lo supportano
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(metrics_server PRIVATE -fdiagnostics-color=always)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(metrics_server PRIVATE -fcolor-diagnostics)
endif()

# Statistiche di compilazione
option(ENABLE_BUILD_STATS "Show build statistics" OFF)
if(ENABLE_BUILD_STATS)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(metrics_server PRIVATE -ftime-report)
    endif()
endif()

# ============================================================================
# SUPPORTO PER CPU SPECIFICHE (opzionale, decommentare se necessario)
# ============================================================================

# # Per CPU Intel con AVX2
# option(ENABLE_AVX2 "Enable AVX2 instructions" OFF)
# if(ENABLE_AVX2)
#     target_compile_options(metrics_server PRIVATE -mavx2 -mfma)
# endif()

# # Per CPU Intel con AVX-512
# option(ENABLE_AVX512 "Enable AVX-512 instructions" OFF)
# if(ENABLE_AVX512)
#     target_compile_options(metrics_server PRIVATE -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl)
# endif()

# ============================================================================
# STAMPA CONFIGURAZIONE
# ============================================================================

message(STATUS "")
message(STATUS "=== Metrics Server Build Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Optimizations: ENABLED (O3, native arch, LTO)")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug mode: ENABLED (with sanitizers)")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    message(STATUS "Release with Debug Info: ENABLED")
elseif(CMAKE_BUILD_TYPE STREQUAL "Profile")
    message(STATUS "Profile mode: ENABLED")
endif()

message(STATUS "==========================================")
message(STATUS "")

# Copia script di generazione certificati
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/generate_certs.sh
    ${CMAKE_CURRENT_BINARY_DIR}/generate_certs.sh
    COPYONLY
)