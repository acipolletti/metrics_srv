#!/bin/bash
# build.sh - Script di build ottimizzato per il server metriche

set -e

# Colori per output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Build Server Metriche C++20 ===${NC}"
echo

# Verifica dipendenze
echo -e "${YELLOW}Verifica dipendenze...${NC}"

# Verifica compilatore C++20
if command -v g++ &> /dev/null; then
    GXX_VERSION=$(g++ --version | head -n1 | awk '{print $3}')
    echo -e "${GREEN}✓ g++ trovato: versione $GXX_VERSION${NC}"
else
    echo -e "${RED}✗ g++ non trovato. Installa con: sudo apt-get install g++${NC}"
    exit 1
fi

# Verifica OpenSSL
if pkg-config --exists openssl; then
    OPENSSL_VERSION=$(pkg-config --modversion openssl)
    echo -e "${GREEN}✓ OpenSSL trovato: versione $OPENSSL_VERSION${NC}"
else
    echo -e "${RED}✗ OpenSSL non trovato. Installa con: sudo apt-get install libssl-dev${NC}"
    exit 1
fi

# Verifica zlib
if pkg-config --exists zlib; then
    ZLIB_VERSION=$(pkg-config --modversion zlib)
    echo -e "${GREEN}✓ zlib trovato: versione $ZLIB_VERSION${NC}"
else
    echo -e "${RED}✗ zlib non trovato. Installa con: sudo apt-get install zlib1g-dev${NC}"
    exit 1
fi

# Verifica nlohmann/json
if [ -f /usr/include/nlohmann/json.hpp ]; then
    echo -e "${GREEN}✓ nlohmann/json trovato${NC}"
else
    echo -e "${YELLOW}⚠ nlohmann/json non trovato nel sistema${NC}"
    echo "  Provo a installarlo..."
    if command -v apt-get &> /dev/null; then
        sudo apt-get install -y nlohmann-json3-dev
    else
        echo -e "${YELLOW}  Scaricalo manualmente da: https://github.com/nlohmann/json${NC}"
    fi
fi

# Verifica/scarica cpp-httplib
if [ ! -f httplib.h ]; then
    echo -e "${YELLOW}Scaricamento cpp-httplib...${NC}"
    wget -q https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ cpp-httplib scaricato${NC}"
    else
        echo -e "${RED}✗ Errore download cpp-httplib${NC}"
        exit 1
    fi
else
    echo -e "${GREEN}✓ cpp-httplib presente${NC}"
fi

echo
echo -e "${YELLOW}Configurazione build...${NC}"

# Flags di compilazione
CXX="g++"
CXXFLAGS="-std=c++20 -O3 -Wall -Wextra -Wpedantic"
DEFINES="-DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_ZLIB_SUPPORT -DCPPHTTPLIB_THREAD_POOL_COUNT=16"
INCLUDES="-I."
LIBS="-lssl -lcrypto -pthread -lz"

# Aggiungi include per nlohmann/json se necessario
if pkg-config --exists nlohmann_json; then
    INCLUDES="$INCLUDES $(pkg-config --cflags nlohmann_json)"
fi

# Build mode
BUILD_MODE="${1:-release}"
if [ "$BUILD_MODE" = "debug" ]; then
    CXXFLAGS="$CXXFLAGS -g -DDEBUG"
    echo -e "${YELLOW}Build mode: DEBUG${NC}"
else
    CXXFLAGS="$CXXFLAGS -DNDEBUG"
    echo -e "${YELLOW}Build mode: RELEASE${NC}"
fi

echo
echo -e "${YELLOW}Compilazione server...${NC}"

# Compila server
$CXX $CXXFLAGS $DEFINES $INCLUDES -o metrics_server metrics_server.cpp $LIBS

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Server compilato con successo${NC}"
else
    echo -e "${RED}✗ Errore compilazione server${NC}"
    exit 1
fi

# Compila test client se esiste
if [ -f test_client.cpp ]; then
    echo
    echo -e "${YELLOW}Compilazione test client...${NC}"
    $CXX $CXXFLAGS $DEFINES $INCLUDES -o test_client test_client.cpp $LIBS
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ Test client compilato con successo${NC}"
    else
        echo -e "${RED}✗ Errore compilazione test client${NC}"
    fi
fi

echo
echo -e "${GREEN}=== Build Completata ===${NC}"
echo

# Verifica certificati
if [ ! -f server.crt ] || [ ! -f server.key ]; then
    echo -e "${YELLOW}Certificati non trovati. Generali con:${NC}"
    echo "  ./generate_certs.sh"
    echo
fi

# Mostra istruzioni di avvio
echo -e "${BLUE}Per avviare il server:${NC}"
echo "  ./metrics_server --api-key mysecretkey"
echo
echo -e "${BLUE}Per vedere tutte le opzioni:${NC}"
echo "  ./metrics_server --help"
echo

# Test delle funzionalità di compressione
echo -e "${YELLOW}Verifica supporto compressione...${NC}"
cat > test_compression.cpp << 'EOF'
#include <iostream>
#define CPPHTTPLIB_ZLIB_SUPPORT
#include "httplib.h"

int main() {
    std::cout << "✓ Supporto gzip abilitato" << std::endl;
    return 0;
}
EOF

$CXX -std=c++20 -o test_compression test_compression.cpp -lz 2>/dev/null
if [ -f test_compression ]; then
    ./test_compression
    rm -f test_compression test_compression.cpp
else
    echo -e "${YELLOW}⚠ Compressione gzip potrebbe non essere disponibile${NC}"
    rm -f test_compression.cpp
fi

echo
echo -e "${GREEN}Build completata con successo! 🚀${NC}"