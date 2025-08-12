# Makefile for Parquet Metrics Writer Testing
# Usage: make [target]

CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O3 -march=native -pthread
INCLUDES = -I. -I/usr/local/include
LDFLAGS = -L/usr/local/lib
LIBS = -larrow -lparquet -lpthread -lssl -lcrypto

# Target executables
TARGETS = test_parquet metrics_server_parquet

# Default target
all: $(TARGETS)

# Test program for ParquetMetricsWriter
test_parquet: example_parquet_integration.cpp ParquetMetricsWriter.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ example_parquet_integration.cpp $(LDFLAGS) $(LIBS)
	@echo "Built: $@"
	@echo "Run with: ./$@"

# Full metrics server with Parquet support
metrics_server_parquet: metrics_server.cpp ParquetMetricsWriter.hpp TelegrafToRedisTS.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DENABLE_PARQUET_STORAGE -o $@ metrics_server.cpp $(LDFLAGS) $(LIBS) -lhttplib
	@echo "Built: $@"
	@echo "Run with: ./$@ [--http-only|--https-only]"

# Quick test - compile and run
test: test_parquet
	@echo "\n=== Running Parquet Tests ==="
	./test_parquet

# Benchmark
benchmark: test_parquet
	@echo "\n=== Running Performance Benchmark ==="
	time ./test_parquet

# Check dependencies
check-deps:
	@echo "Checking dependencies..."
	@command -v pkg-config >/dev/null 2>&1 || { echo "pkg-config not found"; exit 1; }
	@pkg-config --exists arrow || { echo "Apache Arrow not found"; exit 1; }
	@pkg-config --exists parquet || { echo "Apache Parquet not found"; exit 1; }
	@echo "All dependencies found!"
	@echo "Arrow version: $$(pkg-config --modversion arrow)"
	@echo "Parquet version: $$(pkg-config --modversion parquet)"

# Install Arrow/Parquet (Ubuntu/Debian)
install-arrow:
	@echo "Installing Apache Arrow and Parquet..."
	wget -qO- https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest.deb | sudo apt install -y
	sudo apt update
	sudo apt install -y libarrow-dev libparquet-dev libarrow-dataset-dev

# Create test data
test-data:
	@echo "Creating test metrics data..."
	@mkdir -p metrics_data
	@echo '{"metrics":[{"name":"cpu","timestamp":1700000000,"value":50.5,"tags":{"host":"test"}}]}' > test_metrics.json

# Clean build artifacts
clean:
	rm -f $(TARGETS)
	rm -f *.o

# Clean all data
clean-all: clean
	rm -rf metrics_data/
	rm -rf production_metrics/
	rm -f *.parquet
	rm -f metrics.jsonl

# View Parquet files with parquet-tools
view-parquet:
	@if command -v parquet-tools >/dev/null 2>&1; then \
		find metrics_data -name "*.parquet" -exec echo "File: {}" \; -exec parquet-tools show {} \; | head -50; \
	else \
		echo "parquet-tools not installed. Install with: pip install parquet-tools"; \
	fi

# Statistics
stats:
	@echo "\n=== Parquet Storage Statistics ==="
	@echo "Number of files: $$(find metrics_data -name "*.parquet" 2>/dev/null | wc -l)"
	@echo "Total size: $$(du -sh metrics_data 2>/dev/null | cut -f1)"
	@echo "Latest file: $$(ls -t metrics_data/**/*.parquet 2>/dev/null | head -1)"
	@if [ -f "$$(ls -t metrics_data/**/*.parquet 2>/dev/null | head -1)" ]; then \
		echo "Latest file size: $$(ls -lh $$(ls -t metrics_data/**/*.parquet 2>/dev/null | head -1) | awk '{print $$5}')"; \
	fi

# Help
help:
	@echo "Parquet Metrics Writer - Makefile Targets"
	@echo ""
	@echo "Build targets:"
	@echo "  make all              - Build all targets"
	@echo "  make test_parquet     - Build test program"
	@echo "  make metrics_server_parquet - Build full server"
	@echo ""
	@echo "Test targets:"
	@echo "  make test            - Run tests"
	@echo "  make benchmark       - Run performance benchmark"
	@echo "  make test-data       - Create test data"
	@echo ""
	@echo "Utility targets:"
	@echo "  make check-deps      - Check dependencies"
	@echo "  make install-arrow   - Install Arrow/Parquet libraries"
	@echo "  make view-parquet    - View Parquet files content"
	@echo "  make stats           - Show storage statistics"
	@echo "  make clean           - Clean build artifacts"
	@echo "  make clean-all       - Clean everything including data"
	@echo "  make help            - Show this help"

.PHONY: all test benchmark check-deps install-arrow test-data clean clean-all view-parquet stats help