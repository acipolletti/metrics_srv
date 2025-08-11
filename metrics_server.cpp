#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

// Include per il server HTTP
#include <httplib.h>  // cpp-httplib
#include <nlohmann/json.hpp>
#include "TelegrafToRedisTS.hpp"

using json = nlohmann::json;
using namespace std;
using namespace std::chrono;

class MetricsServer {
private:
    httplib::Server server;
    httplib::SSLServer ssl_server;
    
    // Struttura per un elemento della queue
    struct QueueItem {
        string raw_data;        // Body HTTP raw (non parsato)
        string source_ip;       // IP del client
        steady_clock::time_point received_time;  // Timestamp ricezione
    };
    
    // Queue asincrona per processamento - contiene STRING non JSON!
    queue<QueueItem> metricsQueue;
    mutex queueMutex;
    condition_variable queueCV;
    atomic<bool> shouldStop{false};
    
    // Statistiche dettagliate
    atomic<size_t> total_requests_received{0};
    atomic<size_t> total_metrics_received{0};
    atomic<size_t> total_metrics_processed{0};
    atomic<size_t> total_parse_errors{0};
    atomic<size_t> metrics_in_queue{0};
    atomic<size_t> total_processing_time_ms{0};
    atomic<size_t> max_queue_size{0};
    
    // Configurazione Redis
    vector<unique_ptr<RedisTimeSeriesGenerator>> generators;  // Un generator per thread
    RedisTimeSeriesGenerator::Config config;
    
    // Thread pool per processamento
    vector<thread> processingThreads;
    static constexpr size_t NUM_WORKERS = 8;  // Aumentato per migliore parallelismo
    static constexpr size_t QUEUE_WARNING_THRESHOLD = 10000;
    static constexpr size_t QUEUE_MAX_SIZE = 100000;  // Limite massimo queue
    
    // File logging (opzionale)
    ofstream metrics_file;
    mutex file_mutex;
    bool enableFileLogging = true;
    
    // Metriche di performance per thread
    struct ThreadMetrics {
        atomic<size_t> items_processed{0};
        atomic<size_t> parse_errors{0};
        atomic<size_t> total_time_ms{0};
    };
    vector<ThreadMetrics> threadMetrics;

public:
    MetricsServer(const string& cert_file, const string& key_file) 
        : ssl_server(cert_file.c_str(), key_file.c_str()),
          threadMetrics(NUM_WORKERS) {
        
        // Configura il generatore Redis
        config.createTimeSeries = true;
        config.useMultiAdd = true;  // Batch processing
        config.retention = 86400;
        config.keyPrefix = "metrics";
        config.duplicatePolicyValue = "LAST";
        
        // Crea un generator per ogni worker thread (evita contention)
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            generators.push_back(make_unique<RedisTimeSeriesGenerator>(config));
        }
        
        // Apri file per logging solo se abilitato
        if (enableFileLogging) {
            metrics_file.open("metrics.jsonl", ios::app);
        }
        
        // Setup dei route
        setupRoutes(server);
        setupRoutes(ssl_server);
        
        // Avvia i thread di processamento
        startProcessingThreads();
        
        cout << "=== Metrics Server Initialized ===" << endl;
        cout << "Worker threads: " << NUM_WORKERS << endl;
        cout << "Queue max size: " << QUEUE_MAX_SIZE << endl;
        cout << "File logging: " << (enableFileLogging ? "enabled" : "disabled") << endl;
    }
    
    ~MetricsServer() {
        cout << "Shutting down metrics server..." << endl;
        
        // Ferma i thread di processamento
        shouldStop = true;
        queueCV.notify_all();
        
        for (auto& t : processingThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        
        // Processa eventuali metriche rimanenti nella queue
        if (!metricsQueue.empty()) {
            cout << "Warning: " << metricsQueue.size() << " items still in queue at shutdown" << endl;
        }
        
        if (metrics_file.is_open()) {
            metrics_file.close();
        }
        
        // Stampa statistiche finali
        printFinalStats();
    }
    
    template<typename ServerType>
    void setupRoutes(ServerType& srv) {
        // Configura il server per migliori performance
        srv.set_keep_alive_max_count(1000);
        srv.set_keep_alive_timeout(5);
        srv.set_read_timeout(5, 0);
        srv.set_write_timeout(5, 0);
        srv.set_payload_max_length(100 * 1024 * 1024); // 100MB max
        
        // Endpoint principale - ULTRA VELOCE, solo enqueue
        srv.Post("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
            handleMetricsAsync(req, res);
        });
        
        // Health check con metriche dettagliate
        srv.Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
            auto queue_size = metrics_in_queue.load();
            string status = "healthy";
            if (queue_size > QUEUE_WARNING_THRESHOLD) {
                status = "warning";
            }
            if (queue_size > QUEUE_MAX_SIZE * 0.9) {
                status = "critical";
            }
            
            json response = {
                {"status", status},
                {"timestamp", getCurrentTimestamp()},
                {"queue_size", queue_size},
                {"max_queue_size_seen", max_queue_size.load()},
                {"total_requests", total_requests_received.load()},
                {"total_metrics_processed", total_metrics_processed.load()},
                {"workers", NUM_WORKERS}
            };
            res.set_content(response.dump(), "application/json");
        });
        
        // Statistiche dettagliate per thread
        srv.Get("/metrics/stats", [this](const httplib::Request& req, httplib::Response& res) {
            json thread_stats = json::array();
            for (size_t i = 0; i < NUM_WORKERS; ++i) {
                thread_stats.push_back({
                    {"thread_id", i},
                    {"items_processed", threadMetrics[i].items_processed.load()},
                    {"parse_errors", threadMetrics[i].parse_errors.load()},
                    {"avg_time_ms", threadMetrics[i].items_processed > 0 ? 
                        threadMetrics[i].total_time_ms.load() / threadMetrics[i].items_processed.load() : 0}
                });
            }
            
            json response = {
                {"total_requests_received", total_requests_received.load()},
                {"total_metrics_received", total_metrics_received.load()},
                {"total_metrics_processed", total_metrics_processed.load()},
                {"total_parse_errors", total_parse_errors.load()},
                {"current_queue_size", metrics_in_queue.load()},
                {"max_queue_size_seen", max_queue_size.load()},
                {"avg_processing_time_ms", total_metrics_processed > 0 ?
                    total_processing_time_ms.load() / total_metrics_processed.load() : 0},
                {"thread_stats", thread_stats},
                {"timestamp", getCurrentTimestamp()}
            };
            res.set_content(response.dump(4), "application/json");
        });
        
        // Endpoint per flush forzato della queue
        srv.Post("/admin/flush", [this](const httplib::Request& req, httplib::Response& res) {
            queueCV.notify_all();  // Sveglia tutti i worker
            json response = {
                {"status", "flushing"},
                {"queue_size", metrics_in_queue.load()}
            };
            res.set_content(response.dump(), "application/json");
        });
    }
    
    void handleMetricsAsync(const httplib::Request& req, httplib::Response& res) {
        // CRITICO: Questa funzione deve essere VELOCISSIMA
        // Non fare parsing, validazione o processing qui!
        
        auto start_time = steady_clock::now();
        total_requests_received++;
        
        // Controlla dimensione queue (veloce)
        size_t current_queue_size = metrics_in_queue.load();
        if (current_queue_size >= QUEUE_MAX_SIZE) {
            // Queue piena, rifiuta la richiesta
            json error_response = {
                {"status", "error"},
                {"message", "Queue full, server overloaded"},
                {"queue_size", current_queue_size}
            };
            res.status = 503;  // Service Unavailable
            res.set_content(error_response.dump(), "application/json");
            return;
        }
        
        // Crea l'item per la queue - SOLO copia della stringa!
        QueueItem item{
            req.body,           // Copia il body raw (stringa)
            req.remote_addr,    // IP del client
            start_time          // Timestamp
        };
        
        // Aggiungi alla queue (operazione velocissima)
        {
            lock_guard<mutex> lock(queueMutex);
            metricsQueue.push(move(item));  // Move semantics per efficienza
            current_queue_size = metricsQueue.size();
            metrics_in_queue = current_queue_size;
            
            // Aggiorna max queue size
            size_t prev_max = max_queue_size.load();
            while (current_queue_size > prev_max && 
                   !max_queue_size.compare_exchange_weak(prev_max, current_queue_size));
        }
        
        // Notifica UN worker thread
        queueCV.notify_one();
        
        // Risposta immediata - NON include count perché non abbiamo parsato!
        json response = {
            {"status", "accepted"},
            {"message", "Metrics queued for processing"},
            {"queue_position", current_queue_size}
        };
        
        res.status = 202;  // HTTP 202 Accepted
        res.set_content(response.dump(), "application/json");
        
        // Tempo di risposta dovrebbe essere < 1ms
        auto duration = duration_cast<microseconds>(steady_clock::now() - start_time).count();
        if (duration > 1000) {  // > 1ms
            cout << "Warning: Slow enqueue time: " << duration << " microseconds" << endl;
        }
    }
    
    void startProcessingThreads() {
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            processingThreads.emplace_back([this, i]() {
                processMetricsWorker(i);
            });
            
            // Imposta thread affinity (opzionale, per Linux)
            #ifdef __linux__
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(i % thread::hardware_concurrency(), &cpuset);
            pthread_setaffinity_np(processingThreads.back().native_handle(), 
                                  sizeof(cpu_set_t), &cpuset);
            #endif
        }
    }
    
    void processMetricsWorker(size_t workerId) {
        cout << "Worker " << workerId << " started (TID: " << this_thread::get_id() << ")" << endl;
        
        // Generator dedicato per questo thread
        auto& generator = generators[workerId];
        
        // Buffer locale per batch processing
        vector<QueueItem> localBatch;
        localBatch.reserve(100);
        
        while (!shouldStop) {
            unique_lock<mutex> lock(queueMutex);
            
            // Aspetta che ci siano metriche o timeout per batch processing
            auto timeout_point = chrono::steady_clock::now() + chrono::milliseconds(100);
            queueCV.wait_until(lock, timeout_point, [this]() {
                return !metricsQueue.empty() || shouldStop;
            });
            
            if (shouldStop && metricsQueue.empty()) break;
            
            // Prendi un batch di elementi dalla queue
            localBatch.clear();
            const size_t BATCH_SIZE = 50;  // Processa fino a 50 elementi per volta
            
            while (!metricsQueue.empty() && localBatch.size() < BATCH_SIZE) {
                localBatch.push_back(move(metricsQueue.front()));
                metricsQueue.pop();
            }
            metrics_in_queue = metricsQueue.size();
            
            lock.unlock();  // IMPORTANTE: Rilascia il lock prima del processing!
            
            // Processa il batch locale SENZA lock
            for (const auto& item : localBatch) {
                processRawMetrics(workerId, item, *generator);
            }
            
            // Aggiorna statistiche del thread
            threadMetrics[workerId].items_processed += localBatch.size();
        }
        
        cout << "Worker " << workerId << " stopped. Processed " 
             << threadMetrics[workerId].items_processed << " items" << endl;
    }
    
    void processRawMetrics(size_t workerId, const QueueItem& item, 
                           RedisTimeSeriesGenerator& generator) {
        auto start_time = steady_clock::now();
        
        try {
            // ORA facciamo il parsing JSON (nel worker thread, non nell'handler HTTP!)
            json data = json::parse(item.raw_data);
            
            // Verifica e processa metriche
            if (data.contains("metrics") && data["metrics"].is_array()) {
                const auto& metrics = data["metrics"];
                size_t metrics_count = metrics.size();
                
                // Genera comandi Redis
                auto commands = generator.generateCommands(metrics);
                
                // TODO: Invia i comandi a Redis
                // Per ora solo conteggio
                total_metrics_processed += metrics_count;
                total_metrics_received += metrics_count;
                
                // Log su file se abilitato (con lock per thread safety)
                if (enableFileLogging && metrics_file.is_open()) {
                    json log_entry = {
                        {"timestamp", getCurrentTimestamp()},
                        {"worker_id", workerId},
                        {"source_ip", item.source_ip},
                        {"metrics_count", metrics_count},
                        {"queue_delay_ms", duration_cast<milliseconds>(
                            steady_clock::now() - item.received_time).count()}
                    };
                    
                    lock_guard<mutex> lock(file_mutex);
                    metrics_file << log_entry.dump() << endl;
                }
                
                // Debug output (disabilita in produzione)
                if (metrics_count > 0 && workerId == 0) {  // Solo worker 0 per ridurre output
                    static atomic<size_t> debug_counter{0};
                    if (++debug_counter % 100 == 0) {  // Ogni 100 batch
                        cout << "[Worker " << workerId << "] Processed batch: " 
                             << metrics_count << " metrics, "
                             << "Queue: " << metrics_in_queue.load() << endl;
                    }
                }
            }
            
        } catch (const json::parse_error& e) {
            // Errore di parsing JSON
            threadMetrics[workerId].parse_errors++;
            total_parse_errors++;
            
            cerr << "[Worker " << workerId << "] JSON parse error: " << e.what() << endl;
            
            // Log dell'errore
            if (enableFileLogging && metrics_file.is_open()) {
                json error_log = {
                    {"timestamp", getCurrentTimestamp()},
                    {"worker_id", workerId},
                    {"error", "parse_error"},
                    {"message", e.what()},
                    {"data_size", item.raw_data.size()}
                };
                
                lock_guard<mutex> lock(file_mutex);
                metrics_file << error_log.dump() << endl;
            }
        } catch (const exception& e) {
            cerr << "[Worker " << workerId << "] Processing error: " << e.what() << endl;
        }
        
        // Aggiorna tempo di processing
        auto duration_ms = duration_cast<milliseconds>(steady_clock::now() - start_time).count();
        threadMetrics[workerId].total_time_ms += duration_ms;
        total_processing_time_ms += duration_ms;
    }
    
    void printFinalStats() {
        cout << "\n=== Final Statistics ===" << endl;
        cout << "Total requests received: " << total_requests_received << endl;
        cout << "Total metrics received: " << total_metrics_received << endl;
        cout << "Total metrics processed: " << total_metrics_processed << endl;
        cout << "Total parse errors: " << total_parse_errors << endl;
        cout << "Max queue size reached: " << max_queue_size << endl;
        cout << "Average processing time: " 
             << (total_metrics_processed > 0 ? 
                 total_processing_time_ms.load() / total_metrics_processed.load() : 0) 
             << " ms" << endl;
        
        cout << "\nPer-thread statistics:" << endl;
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            cout << "  Thread " << i << ": " 
                 << threadMetrics[i].items_processed << " items, "
                 << threadMetrics[i].parse_errors << " errors" << endl;
        }
    }
    
    void runHTTP(const string& host, int port) {
        cout << "\n=== HTTP Server Configuration ===" << endl;
        cout << "Address: http://" << host << ":" << port << endl;
        cout << "Worker threads: " << NUM_WORKERS << endl;
        cout << "Max queue size: " << QUEUE_MAX_SIZE << endl;
        cout << "Warning threshold: " << QUEUE_WARNING_THRESHOLD << endl;
        cout << "==================================\n" << endl;
        
        server.listen(host.c_str(), port);
    }
    
    void runHTTPS(const string& host, int port) {
        cout << "\n=== HTTPS Server Configuration ===" << endl;
        cout << "Address: https://" << host << ":" << port << endl;
        cout << "Worker threads: " << NUM_WORKERS << endl;
        cout << "Max queue size: " << QUEUE_MAX_SIZE << endl;
        cout << "Warning threshold: " << QUEUE_WARNING_THRESHOLD << endl;
        cout << "===================================\n" << endl;
        
        ssl_server.listen(host.c_str(), port);
    }
    
private:
    static string getCurrentTimestamp() {
        auto now = chrono::system_clock::now();
        auto time_t = chrono::system_clock::to_time_t(now);
        
        stringstream ss;
        ss << put_time(gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
        return ss.str();
    }
};

int main(int argc, char* argv[]) {
    try {
        // Ottimizzazioni a livello di sistema
        #ifdef __linux__
        // Aumenta la priorità del processo
        nice(-10);
        #endif
        
        string mode = "https";
        if (argc > 1) {
            mode = argv[1];
        }
        
        cout << "Starting Metrics Server v2.0 (Fully Async)" << endl;
        cout << "CPU cores available: " << thread::hardware_concurrency() << endl;
        
        MetricsServer server("server.crt", "server.key");
        
        if (mode == "--http-only") {
            server.runHTTP("0.0.0.0", 8080);
        } else if (mode == "--https-only") {
            server.runHTTPS("0.0.0.0", 8443);
        } else {
            // Default: prova HTTPS, fallback a HTTP
            try {
                server.runHTTPS("0.0.0.0", 8443);
            } catch (const exception& e) {
                cerr << "HTTPS failed: " << e.what() << endl;
                cerr << "Starting HTTP server..." << endl;
                server.runHTTP("0.0.0.0", 8080);
            }
        }
        
    } catch (const exception& e) {
        cerr << "Fatal error: " << e.what() << endl;
        return 1;
    }
    
    return 0;
}