# Makefile per Server Metriche C++20

# Compilatore e flags
CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic
DEFINES := -DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_ZLIB_SUPPORT -DCPPHTTPLIB_THREAD_POOL_COUNT=16
LDFLAGS := -lssl -lcrypto -pthread -lz

# Modalità di build (release di default)
BUILD_MODE ?= release

ifeq ($(BUILD_MODE),debug)
    CXXFLAGS += -g -O0 -DDEBUG
else
    CXXFLAGS += -O3 -DNDEBUG
endif

# File sorgenti e target
SERVER_SRC := metrics_server.cpp
CLIENT_SRC := test_client.cpp
SSL_CLIENT_SRC := simple_ssl_client.cpp
SERVER_BIN := metrics_server
CLIENT_BIN := test_client
SSL_CLIENT_BIN := simple_ssl_client

# Regole
.PHONY: all clean server client test certs run help

# Build di default
all: server client ssl-client

# Server principale
server: $(SERVER_BIN)

$(SERVER_BIN): $(SERVER_SRC) httplib.h
	@echo "🔨 Building server ($(BUILD_MODE) mode)..."
	$(CXX) $(CXXFLAGS) $(DEFINES) -o $@ $< $(LDFLAGS)
	@echo "✅ Server built successfully!"

# Client di test
client: $(CLIENT_BIN)

$(CLIENT_BIN): $(CLIENT_SRC) httplib.h
	@echo "🔨 Building test client..."
	$(CXX) $(CXXFLAGS) $(DEFINES) -o $@ $< $(LDFLAGS)
	@echo "✅ Test client built successfully!"

# SSL Client semplice
ssl-client: $(SSL_CLIENT_BIN)

$(SSL_CLIENT_BIN): $(SSL_CLIENT_SRC)
	@echo "🔨 Building simple SSL client..."
	@if [ -f $(SSL_CLIENT_SRC) ]; then \
		$(CXX) $(CXXFLAGS) -o $@ $< -lssl -lcrypto; \
		echo "✅ SSL client built successfully!"; \
	else \
		echo "⚠️  $(SSL_CLIENT_SRC) not found, skipping"; \
	fi

# Scarica cpp-httplib se non presente
httplib.h:
	@echo "📥 Downloading cpp-httplib..."
	@wget -q https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
	@echo "✅ cpp-httplib downloaded!"

# Genera certificati
certs:
	@echo "🔐 Generating certificates..."
	@chmod +x generate_certs.sh
	@./generate_certs.sh
	@echo "✅ Certificates generated!"

# Esegui il server
run: server certs
	@echo "🚀 Starting server..."
	./$(SERVER_BIN) --api-key development-key

# Test base
test: all
	@echo "🧪 Running basic tests..."
	@if [ -x ./$(CLIENT_BIN) ]; then \
		./$(CLIENT_BIN) --test --api-key development-key; \
	else \
		echo "⚠️  Test client non disponibile, usa test-curl invece"; \
	fi

# Test con curl (più affidabile per certificati client)
test-curl: certs
	@echo "🧪 Running tests with cURL..."
	@chmod +x test_with_curl.sh
	@API_KEY=development-key ./test_with_curl.sh

# Test con SSL client
test-ssl: ssl-client certs
	@echo "🧪 Running tests with SSL client..."
	@if [ -x ./$(SSL_CLIENT_BIN) ]; then \
		./$(SSL_CLIENT_BIN) --api-key development-key; \
	else \
		echo "⚠️  SSL client not available"; \
	fi

# Test streaming
test-streaming: all
	@echo "🌊 Testing streaming..."
	./$(CLIENT_BIN) --streaming 10 --api-key development-key --no-verify-ssl

# Test compressione
test-compression: all
	@echo "📦 Testing compression..."
	./$(CLIENT_BIN) --compression --api-key development-key --no-verify-ssl

# Pulizia
clean:
	@echo "🧹 Cleaning..."
	@rm -f $(SERVER_BIN) $(CLIENT_BIN) $(SSL_CLIENT_BIN)
	@rm -f *.o *.log
	@echo "✅ Cleaned!"

# Help
help:
	@echo "📚 Makefile per Server Metriche C++20"
	@echo ""
	@echo "Targets disponibili:"
	@echo "  make all              - Compila server e client"
	@echo "  make server           - Compila solo il server"
	@echo "  make client           - Compila solo il client"
	@echo "  make ssl-client       - Compila client SSL semplice"
	@echo "  make certs            - Genera certificati SSL"
	@echo "  make run              - Avvia il server"
	@echo "  make test             - Esegui test di base"
	@echo "  make test-curl        - Test completi con cURL"
	@echo "  make test-ssl         - Test con SSL client"
	@echo "  make test-streaming   - Test streaming SSE"
	@echo "  make test-compression - Test compressione gzip"
	@echo "  make clean            - Pulisci file compilati"
	@echo ""
	@echo "Opzioni:"
	@echo "  BUILD_MODE=debug      - Compila in modalità debug"
	@echo "  BUILD_MODE=release    - Compila in modalità release (default)"
	@echo ""
	@echo "Esempi:"
	@echo "  make BUILD_MODE=debug"
	@echo "  make server BUILD_MODE=release"
	@echo "  make test-curl"

# Installazione dipendenze (richiede sudo)
install-deps:
	@echo "📦 Installing dependencies..."
	@if command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get update && \
		sudo apt-get install -y g++ libssl-dev zlib1g-dev nlohmann-json3-dev; \
	elif command -v yum >/dev/null 2>&1; then \
		sudo yum install -y gcc-c++ openssl-devel zlib-devel; \
	elif command -v brew >/dev/null 2>&1; then \
		brew install openssl zlib nlohmann-json; \
	else \
		echo "❌ Package manager non supportato. Installa manualmente:"; \
		echo "   - g++ (C++20)"; \
		echo "   - OpenSSL dev"; \
		echo "   - zlib dev"; \
		echo "   - nlohmann/json"; \
	fi
	@echo "✅ Dependencies installed!"