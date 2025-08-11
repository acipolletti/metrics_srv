#!/bin/bash
# build.sh - Script ottimizzato per compilare il metrics server

set -e  # Esci in caso di errore

# Colori per output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configurazione default
BUILD_TYPE="Release"
CLEAN_BUILD=false
VERBOSE=false
PARALLEL_JOBS=$(nproc)

# Funzione per stampare con colori
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Funzione per mostrare l'help
show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -t, --type TYPE      Build type: Release (default), Debug, RelWithDebInfo, Profile"
    echo "  -c, --clean          Clean build (rimuove la directory build)"
    echo "  -v, --verbose        Verbose output"
    echo "  -j, --jobs N         Number of parallel jobs (default: $(nproc))"
    echo "  -h, --help           Show this help message"
    echo ""
    echo "Build Types:"
    echo "  Release          - Massime ottimizzazioni, no debug info"
    echo "  Debug            - No ottimizzazioni, con debug info e sanitizers"
    echo "  RelWithDebInfo   - Ottimizzazioni con debug info"
    echo "  Profile          - Ottimizzazioni con simboli per profiling"
    echo ""
    echo "Examples:"
    echo "  $0                           # Build Release"
    echo "  $0 --type Debug --clean      # Clean build Debug"
    echo "  $0 -t Profile -j 8           # Build Profile con 8 jobs"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -j|--jobs)
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Validazione build type
case $BUILD_TYPE in
    Release|Debug|RelWithDebInfo|Profile)
        ;;
    *)
        print_error "Invalid build type: $BUILD_TYPE"
        echo "Valid types: Release, Debug, RelWithDebInfo, Profile"
        exit 1
        ;;
esac

echo ""
echo "======================================"
echo "     METRICS SERVER BUILD SYSTEM     "
echo "======================================"
echo ""

# Mostra configurazione
print_info "Build Configuration:"
echo "  Build Type:     $BUILD_TYPE"
echo "  Parallel Jobs:  $PARALLEL_JOBS"
echo "  Clean Build:    $CLEAN_BUILD"
echo "  Verbose:        $VERBOSE"
echo ""

# Controlla dipendenze
print_info "Checking dependencies..."

check_command() {
    if ! command -v $1 &> /dev/null; then
        print_error "$1 is not installed!"
        exit 1
    fi
}

check_command cmake
check_command make
check_command g++
check_command openssl

# Ottieni versione del compilatore
COMPILER_VERSION=$(g++ --version | head -n1)
print_info "Compiler: $COMPILER_VERSION"

# Controlla versione CMake
CMAKE_VERSION=$(cmake --version | head -n1 | cut -d' ' -f3)
print_info "CMake version: $CMAKE_VERSION"

# Clean build se richiesto
if [ "$CLEAN_BUILD" = true ]; then
    print_warning "Cleaning previous build..."
    rm -rf build
fi

# Crea directory build
mkdir -p build
cd build

# Configura CMake
print_info "Configuring CMake for $BUILD_TYPE build..."

CMAKE_ARGS="-DCMAKE_BUILD_TYPE=$BUILD_TYPE"

if [ "$VERBOSE" = true ]; then
    CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_VERBOSE_MAKEFILE=ON"
fi

# Mostra flags di compilazione per ogni build type
case $BUILD_TYPE in
    Release)
        print_info "Optimization flags: -O3 -march=native -flto -funroll-loops"
        ;;
    Debug)
        print_info "Debug flags: -g -O0 -fsanitize=address,undefined"
        ;;
    RelWithDebInfo)
        print_info "Optimization with debug: -O3 -g -march=native"
        ;;
    Profile)
        print_info "Profile flags: -O3 -g -fno-omit-frame-pointer"
        ;;
esac

cmake .. $CMAKE_ARGS

# Compila
print_info "Building with $PARALLEL_JOBS parallel jobs..."

if [ "$VERBOSE" = true ]; then
    make -j$PARALLEL_JOBS VERBOSE=1
else
    make -j$PARALLEL_JOBS
fi

print_success "Build completed successfully!"

# Genera certificati se non esistono
if [ ! -f "server.crt" ] || [ ! -f "server.key" ]; then
    print_info "Generating self-signed certificates..."
    openssl req -new -newkey rsa:2048 -days 365 -nodes -x509 \
        -subj "/C=IT/ST=Lombardy/L=Milan/O=MyOrg/CN=localhost" \
        -keyout server.key -out server.crt 2>/dev/null
    print_success "Certificates generated!"
fi

# Mostra dimensione eseguibile
EXEC_SIZE=$(du -h metrics_server | cut -f1)
print_info "Executable size: $EXEC_SIZE"

# Strip symbols per Release (opzionale)
if [ "$BUILD_TYPE" = "Release" ]; then
    print_info "Stripping debug symbols from Release build..."
    strip metrics_server
    NEW_SIZE=$(du -h metrics_server | cut -f1)
    print_info "Stripped executable size: $NEW_SIZE"
fi

# Crea script di lancio
cat > run_server.sh << 'EOF'
#!/bin/bash
# Script per lanciare il server con varie opzioni

MODE=${1:-auto}
PORT_HTTP=${2:-8080}
PORT_HTTPS=${3:-8443}

case $MODE in
    http)
        echo "Starting HTTP server on port $PORT_HTTP..."
        ./metrics_server --http-only
        ;;
    https)
        echo "Starting HTTPS server on port $PORT_HTTPS..."
        ./metrics_server --https-only
        ;;
    auto|*)
        echo "Starting server in auto mode..."
        ./metrics_server
        ;;
esac
EOF

chmod +x run_server.sh

echo ""
print_success "=== Build completed successfully! ==="
echo ""
echo "Build artifacts in: $(pwd)"
echo ""
echo "To run the server:"
echo "  HTTPS only:  ./metrics_server --https-only"
echo "  HTTP only:   ./metrics_server --http-only"
echo "  Auto mode:   ./metrics_server"
echo ""
echo "Or use the helper script:"
echo "  ./run_server.sh [http|https|auto]"
echo ""
echo "Test endpoints:"
echo "  curl -k https://localhost:8443/health"
echo "  curl http://localhost:8080/health"
echo "  curl http://localhost:8080/metrics/stats"
echo ""

# Performance tips per build type
case $BUILD_TYPE in
    Release)
        print_info "Performance tips for Release build:"
        echo "  - CPU affinity: taskset -c 0-3 ./metrics_server"
        echo "  - Nice priority: nice -n -10 ./metrics_server"
        echo "  - Monitor: htop -p \$(pidof metrics_server)"
        ;;
    Debug)
        print_warning "Debug build includes AddressSanitizer and UBSanitizer"
        echo "  Set ASAN_OPTIONS=symbolize=1 for better error reports"
        ;;
    Profile)
        print_info "Profile build ready for:"
        echo "  - perf: perf record -g ./metrics_server"
        echo "  - valgrind: valgrind --tool=callgrind ./metrics_server"
        ;;
esac

echo ""
echo "======================================"
cp /home/ac/app/metrics_srv/src/build/metrics_server .
