// metrics_server_with_parquet.cpp
#include <iostream>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "TelegrafToRedisTS.hpp"
#include "ParquetMetricsWriter.hpp"
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

using json = nlohmann::json;
using namespace std;

class MetricsServerWithStorage {
private:
    httplib::Server server;
    httplib::SSLServer ssl_server;
    
    // Storage backends
    unique_ptr<ParquetMetricsWriter> parquet_writer;
    vector<unique_ptr<RedisTimeSeriesGenerator>> redis_generators;
    
    // Processing queue
    struct QueueItem {
        string raw_data;
        string source_ip;
        chrono::steady_clock::time_point received_time;
    };
    
    queue<QueueItem> metricsQueue;
    mutex queueMutex;
    condition_variable queueCV;
    atomic<bool> shouldStop{false};
    
    // Stats
    atomic<size_t> total_requests{0};
    atomic<size_t> total_metrics{0};
    atomic<size_t> metrics_in_queue{0};
    
    // Worker threads
    vector<thread> workers;
    static constexpr size_t NUM_WORKERS = 8;
    
    // Configuration
    struct Config {
        bool enable_redis = true;
        bool enable_parquet = true;
        bool enable_realtime_query = true;
        string parquet_base_path = "./metrics_data";
    } config;

public:
    MetricsServerWithStorage(const string& cert_file, const string& key_file)
        : ssl_server(cert_file.c_str(), key_file.c_str()) {
        
        // Initialize Parquet writer
        if (config.enable_parquet) {
            ParquetMetricsWriter::Config parquet_config;
            parquet_config.base_path = config.parquet_base_path;
            parquet_config.batch_size = 5000;
            parquet_config.time_window_seconds = 3600;  // New file every hour
            parquet_config.max_file_size_mb = 100;
            parquet_config.partition_by_date = true;
            parquet_config.retention_days = 30;
            
            parquet_writer = make_unique<ParquetMetricsWriter>(parquet_config);
            cout << "Parquet storage enabled at: " << parquet_config.base_path << endl;
        }
        
        // Initialize Redis generators (one per worker)
        if (config.enable_redis) {
            RedisTimeSeriesGenerator::Config redis_config;
            redis_config.createTimeSeries = true;
            redis_config.useMultiAdd = true;
            redis_config.retention = 86400;
            
            for (size_t i = 0; i < NUM_WORKERS; ++i) {
                redis_generators.push_back(make_unique<RedisTimeSeriesGenerator>(redis_config));
            }
            cout << "Redis TimeSeries storage enabled" << endl;
        }
        
        setupRoutes(server);
        setupRoutes(ssl_server);
        startWorkers();
    }
    
    ~MetricsServerWithStorage() {
        shouldStop = true;
        queueCV.notify_all();
        
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
        
        if (parquet_writer) {
            parquet_writer->flush();
        }
    }
    
    template<typename ServerType>
    void setupRoutes(ServerType& srv) {
        // Main metrics endpoint
        srv.Post("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
            handleMetrics(req, res);
        });
        
        // Query endpoint for Parquet data
        srv.Get("/query", [this](const httplib::Request& req, httplib::Response& res) {
            handleQuery(req, res);
        });
        
        // Storage stats
        srv.Get("/storage/stats", [this](const httplib::Request& req, httplib::Response& res) {
            json stats = {
                {"total_requests", total_requests.load()},
                {"total_metrics", total_metrics.load()},
                {"queue_size", metrics_in_queue.load()},
                {"storage", {}}
            };
            
            if (parquet_writer) {
                stats["storage"]["parquet"] = parquet_writer->getStats();
            }
            
            res.set_content(stats.dump(4), "application/json");
        });
        
        // Force cleanup old files
        srv.Post("/storage/cleanup", [this](const httplib::Request& req, httplib::Response& res) {
            if (parquet_writer) {
                parquet_writer->cleanupOldFiles();
                res.set_content(R"({"status":"cleanup_started"})", "application/json");
            } else {
                res.status = 404;
                res.set_content(R"({"error":"parquet_not_enabled"})", "application/json");
            }
        });
        
        // Health check
        srv.Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
            json health = {
                {"status", "healthy"},
                {"queue_size", metrics_in_queue.load()},
                {"workers", NUM_WORKERS}
            };
            res.set_content(health.dump(), "application/json");
        });
    }
    
    void handleMetrics(const httplib::Request& req, httplib::Response& res) {
        total_requests++;
        
        // Fast enqueue without parsing
        QueueItem item{
            req.body,
            req.remote_addr,
            chrono::steady_clock::now()
        };
        
        {
            lock_guard<mutex> lock(queueMutex);
            metricsQueue.push(move(item));
            metrics_in_queue = metricsQueue.size();
        }
        
        queueCV.notify_one();
        
        res.status = 202;
        res.set_content(R"({"status":"accepted"})", "application/json");
    }
    
    // Correzione per il metodo handleQuery in metrics_server.cpp
// Sostituisci il metodo handleQuery con questo:

void handleQuery(const httplib::Request& req, httplib::Response& res) {
    // Query parameters - Fixed API usage
    auto metric_name = req.get_param_value("metric");
    auto start_time_str = req.get_param_value("start");
    auto end_time_str = req.get_param_value("end");
    
    // Fixed: get_param_value doesn't have a default parameter in httplib
    std::string format = "json";
    if (req.has_param("format")) {
        format = req.get_param_value("format");
    }
    
    if (!parquet_writer) {
        res.status = 404;
        res.set_content(R"({"error":"parquet_storage_not_enabled"})", "application/json");
        return;
    }
    
    try {
        // Find relevant Parquet files
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(config.parquet_base_path)) {
            if (entry.path().extension() == ".parquet") {
                files.push_back(entry.path().string());
            }
        }
        
        json results = {
            {"query", {
                {"metric", metric_name},
                {"start", start_time_str},
                {"end", end_time_str}
            }},
            {"files_scanned", files.size()},
            {"data", json::array()}
        };
        
        // Query each file (in production, use parallel processing)
        for (const auto& file : files) {
            auto table_result = parquet_writer->queryMetrics(file, metric_name);
            if (table_result.ok()) {
                auto table = table_result.ValueOrDie();
                // Convert table to JSON (simplified)
                results["data"].push_back({
                    {"file", file},
                    {"rows", table->num_rows()}
                });
            }
        }
        
        res.set_content(results.dump(4), "application/json");
        
    } catch (const std::exception& e) {
        res.status = 500;
        json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}
    void startWorkers() {
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            workers.emplace_back([this, i]() {
                processWorker(i);
            });
        }
    }
    
    void processWorker(size_t id) {
        auto& redis_gen = redis_generators[id];
        vector<QueueItem> batch;
        batch.reserve(100);
        
        while (!shouldStop) {
            {
                unique_lock<mutex> lock(queueMutex);
                queueCV.wait_for(lock, chrono::milliseconds(100), [this]() {
                    return !metricsQueue.empty() || shouldStop;
                });
                
                while (!metricsQueue.empty() && batch.size() < 100) {
                    batch.push_back(move(metricsQueue.front()));
                    metricsQueue.pop();
                }
                metrics_in_queue = metricsQueue.size();
            }
            
            // Process batch
            for (const auto& item : batch) {
                try {
                    json data = json::parse(item.raw_data);
                    
                    if (data.contains("metrics") && data["metrics"].is_array()) {
                        size_t count = data["metrics"].size();
                        total_metrics += count;
                        
                        // Write to Parquet
                        if (parquet_writer) {
                            parquet_writer->addMetrics(data);
                        }
                        
                        // Send to Redis
                        if (config.enable_redis && redis_gen) {
                            auto commands = redis_gen->generateCommands(data["metrics"]);
                            // TODO: Execute Redis commands
                        }
                    }
                } catch (const exception& e) {
                    cerr << "Worker " << id << " error: " << e.what() << endl;
                }
            }
            
            batch.clear();
        }
    }
    
    void run(const string& host, int port, bool use_https) {
        cout << "\n=== Metrics Server with Parquet Storage ===" << endl;
        cout << "Storage backends:" << endl;
        cout << "  - Parquet: " << (config.enable_parquet ? "ENABLED" : "DISABLED") << endl;
        cout << "  - Redis TS: " << (config.enable_redis ? "ENABLED" : "DISABLED") << endl;
        cout << "Workers: " << NUM_WORKERS << endl;
        cout << "Address: " << (use_https ? "https://" : "http://") 
             << host << ":" << port << endl;
        cout << "==========================================\n" << endl;
        
        if (use_https) {
            ssl_server.listen(host.c_str(), port);
        } else {
            server.listen(host.c_str(), port);
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        MetricsServerWithStorage server("server.crt", "server.key");
        
        bool use_https = true;
        if (argc > 1 && string(argv[1]) == "--http-only") {
            use_https = false;
        }
        
        server.run("0.0.0.0", use_https ? 8443 : 8080, use_https);
        
    } catch (const exception& e) {
        cerr << "Fatal error: " << e.what() << endl;
        return 1;
    }
    
    return 0;
}