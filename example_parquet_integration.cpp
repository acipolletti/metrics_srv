// example_parquet_integration.cpp
// Esempio di come integrare ParquetMetricsWriter nel tuo metrics server

#include "ParquetMetricsWriter.hpp"
#include <iostream>
#include <thread>
#include <chrono>

// Esempio 1: Utilizzo base
void basicUsageExample() {
    std::cout << "\n=== Basic Usage Example ===" << std::endl;
    
    // Configurazione
    ParquetMetricsWriter::Config config;
    config.base_path = "./metrics_data";
    config.batch_size = 1000;
    config.time_window_seconds = 300;  // Nuovo file ogni 5 minuti
    config.max_file_size_mb = 50;
    config.enable_compression = true;
    config.compression_type = parquet::Compression::SNAPPY;
    config.partition_by_date = true;
    config.retention_days = 7;
    
    // Crea il writer
    ParquetMetricsWriter writer(config);
    
    // Simula metriche Telegraf
    json telegraf_data = {
        {"metrics", json::array({
            {
                {"name", "cpu.usage_percent"},
                {"timestamp", 1700000000},
                {"value", 45.5},
                {"tags", {
                    {"host", "server01"},
                    {"cpu", "cpu0"},
                    {"region", "eu-west-1"}
                }}
            },
            {
                {"name", "mem"},
                {"timestamp", 1700000000},
                {"fields", {
                    {"used_percent", 67.8},
                    {"total", 16777216000},
                    {"used", 11379851264},
                    {"free", 5397364736}
                }},
                {"tags", {
                    {"host", "server01"},
                    {"region", "eu-west-1"}
                }}
            }
        })}
    };
    
    // Aggiungi metriche
    writer.addMetrics(telegraf_data);
    
    // Forza scrittura
    writer.flush();
    
    // Mostra statistiche
    std::cout << "Statistics: " << writer.getStats().dump(2) << std::endl;
}

// Esempio 2: Integrazione nel worker thread del tuo server
void serverWorkerExample() {
    std::cout << "\n=== Server Worker Integration ===" << std::endl;
    
    class MetricsProcessor {
    private:
        std::unique_ptr<ParquetMetricsWriter> parquet_writer;
        
    public:
        MetricsProcessor() {
            ParquetMetricsWriter::Config config;
            config.base_path = "./production_metrics";
            config.batch_size = 5000;
            config.writer_threads = 4;  // 4 thread di scrittura paralleli
            config.partition_by_date = true;
            config.partition_by_metric = false;
            config.retention_days = 30;
            
            parquet_writer = std::make_unique<ParquetMetricsWriter>(config);
        }
        
        void processQueueItem(const std::string& raw_json) {
            try {
                // Parse JSON dal body HTTP
                json data = json::parse(raw_json);
                
                // Scrivi su Parquet (asincrono, non blocca)
                if (parquet_writer) {
                    parquet_writer->addMetrics(data);
                }
                
                // Qui potresti anche inviare a Redis, etc.
                
            } catch (const json::parse_error& e) {
                std::cerr << "Parse error: " << e.what() << std::endl;
            }
        }
        
        json getStorageStats() {
            if (parquet_writer) {
                return parquet_writer->getStats();
            }
            return json::object();
        }
    };
    
    // Simula processing
    MetricsProcessor processor;
    
    // Simula arrivo di metriche
    for (int i = 0; i < 10; ++i) {
        json metrics = {
            {"metrics", json::array({
                {
                    {"name", "test.metric"},
                    {"timestamp", 1700000000 + i},
                    {"value", 100.0 + i},
                    {"tags", {{"host", "test-host"}}}
                }
            })}
        };
        
        processor.processQueueItem(metrics.dump());
    }
    
    std::cout << "Storage stats: " << processor.getStorageStats().dump(2) << std::endl;
}

// Esempio 3: Query dei dati salvati
void queryExample() {
    std::cout << "\n=== Query Example ===" << std::endl;
    
    ParquetMetricsWriter writer;
    
    // Lista file in un range di date
    auto now = std::chrono::system_clock::now();
    auto yesterday = now - std::chrono::hours(24);
    
    auto files = writer.listFiles(yesterday, now);
    std::cout << "Found " << files.size() << " parquet files" << std::endl;
    
    // Query su un file specifico
    if (!files.empty()) {
        auto result = writer.queryMetrics(
            files[0],
            "cpu.usage_percent",  // Filter by metric name
            1700000000,           // Start timestamp
            1700001000            // End timestamp
        );
        
        if (result.ok()) {
            auto table = result.ValueOrDie();
            std::cout << "Query returned " << table->num_rows() << " rows" << std::endl;
            std::cout << "Schema: " << table->schema()->ToString() << std::endl;
        }
    }
}

// Esempio 4: Monitoraggio e manutenzione
void maintenanceExample() {
    std::cout << "\n=== Maintenance Example ===" << std::endl;
    
    ParquetMetricsWriter::Config config;
    config.retention_days = 7;  // Mantieni solo 7 giorni
    
    ParquetMetricsWriter writer(config);
    
    // Thread di monitoraggio
    std::thread monitor_thread([&writer]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            
            auto stats = writer.getStatistics();
            std::cout << "\n=== Periodic Stats ===" << std::endl;
            std::cout << "Records written: " << stats.total_records_written << std::endl;
            std::cout << "Files created: " << stats.total_files_created << std::endl;
            std::cout << "MB written: " << stats.total_bytes_written / (1024.0 * 1024.0) << std::endl;
            std::cout << "Queue size: " << stats.current_queue_size << std::endl;
            
            // Cleanup automatico vecchi file
            size_t deleted = writer.cleanupOldFiles();
            if (deleted > 0) {
                std::cout << "Cleaned up " << deleted << " old files" << std::endl;
            }
        }
    });
    
    // Simula metriche continue
    for (int i = 0; i < 100; ++i) {
        json data = {
            {"metrics", json::array({
                {
                    {"name", "continuous.metric"},
                    {"timestamp", std::chrono::system_clock::now().time_since_epoch().count() / 1000000000},
                    {"value", std::rand() / double(RAND_MAX) * 100},
                    {"tags", {{"host", "monitor-host"}}}
                }
            })}
        };
        
        writer.addMetrics(data);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    monitor_thread.detach();
}

// Esempio 5: Performance testing
void performanceTest() {
    std::cout << "\n=== Performance Test ===" << std::endl;
    
    ParquetMetricsWriter::Config config;
    config.batch_size = 10000;
    config.writer_threads = 8;
    config.enable_compression = true;
    
    ParquetMetricsWriter writer(config);
    
    const size_t NUM_METRICS = 1000000;
    const size_t BATCH_SIZE = 1000;
    
    auto start = std::chrono::steady_clock::now();
    
    for (size_t i = 0; i < NUM_METRICS; i += BATCH_SIZE) {
        json::array metrics_array;
        
        for (size_t j = 0; j < BATCH_SIZE && (i + j) < NUM_METRICS; ++j) {
            metrics_array.push_back({
                {"name", "perf.test.metric"},
                {"timestamp", 1700000000 + i + j},
                {"value", std::rand() / double(RAND_MAX) * 1000},
                {"tags", {
                    {"host", "perf-host-" + std::to_string(j % 10)},
                    {"datacenter", "dc" + std::to_string(j % 3)}
                }}
            });
        }
        
        json data = {{"metrics", metrics_array}};
        writer.addMetrics(data);
    }
    
    writer.flush();
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
    
    auto stats = writer.getStatistics();
    
    std::cout << "\n=== Performance Results ===" << std::endl;
    std::cout << "Total metrics: " << NUM_METRICS << std::endl;
    std::cout << "Time taken: " << duration << " seconds" << std::endl;
    std::cout << "Throughput: " << NUM_METRICS / duration << " metrics/second" << std::endl;
    std::cout << "Files created: " << stats.total_files_created << std::endl;
    std::cout << "Total MB written: " << stats.total_bytes_written / (1024.0 * 1024.0) << std::endl;
    std::cout << "Compression ratio: " << 
        (NUM_METRICS * 100.0) / stats.total_bytes_written << " x" << std::endl;
}

int main() {
    try {
        // Esegui esempi
        basicUsageExample();
        serverWorkerExample();
        queryExample();
        // maintenanceExample();  // Commentato perché ha un loop infinito
        performanceTest();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}