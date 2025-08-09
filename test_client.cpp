// test_client.cpp
// Client di test per il server REST metriche

#include <iostream>
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std::chrono_literals;

class MetricsTestClient {
private:
    std::string server_host_;
    int server_port_;
    std::string api_key_;
    std::string ca_cert_;
    std::string client_cert_;
    std::string client_key_;
    std::unique_ptr<httplib::SSLClient> client_;
    
    // Generatore numeri casuali
    std::mt19937 rng_{std::random_device{}()};
    std::uniform_real_distribution<double> cpu_dist_{0.0, 100.0};
    std::uniform_real_distribution<double> memory_dist_{1000.0, 8000.0};
    std::uniform_real_distribution<double> disk_dist_{0.0, 100.0};
    std::uniform_int_distribution<int> response_time_dist_{10, 500};
    
    void log(const std::string& message) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::cout << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") 
                  << "] " << message << std::endl;
    }
    
    json generate_system_metrics(const std::string& hostname) {
        return {
            {"name", "system"},
            {"tags", {
                {"host", hostname},
                {"region", "eu-south"},
                {"datacenter", "milan-1"}
            }},
            {"fields", {
                {"cpu_usage", cpu_dist_(rng_)},
                {"memory_used", memory_dist_(rng_)},
                {"memory_total", 8192.0},
                {"disk_usage", disk_dist_(rng_)},
                {"load_1m", cpu_dist_(rng_) / 25.0},
                {"load_5m", cpu_dist_(rng_) / 30.0},
                {"load_15m", cpu_dist_(rng_) / 35.0}
            }},
            {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };
    }
    
    json generate_app_metrics(const std::string& app_name) {
        return {
            {"name", "application"},
            {"tags", {
                {"app", app_name},
                {"version", "1.0.0"},
                {"environment", "production"}
            }},
            {"fields", {
                {"requests_per_second", std::uniform_real_distribution<double>{10.0, 100.0}(rng_)},
                {"response_time_ms", static_cast<double>(response_time_dist_(rng_))},
                {"error_rate", std::uniform_real_distribution<double>{0.0, 5.0}(rng_)},
                {"active_connections", std::uniform_int_distribution<int>{10, 200}(rng_)}
            }}
        };
    }
    
    json generate_network_metrics(const std::string& interface_name) {
        return {
            {"name", "network"},
            {"tags", {
                {"interface", interface_name},
                {"type", "ethernet"}
            }},
            {"fields", {
                {"bytes_sent", std::uniform_real_distribution<double>{1000.0, 1000000.0}(rng_)},
                {"bytes_recv", std::uniform_real_distribution<double>{1000.0, 1000000.0}(rng_)},
                {"packets_sent", std::uniform_real_distribution<double>{100.0, 10000.0}(rng_)},
                {"packets_recv", std::uniform_real_distribution<double>{100.0, 10000.0}(rng_)},
                {"errors", std::uniform_real_distribution<double>{0.0, 10.0}(rng_)}
            }}
        };
    }
    
public:
    MetricsTestClient(const std::string& host, int port) 
        : server_host_(host), server_port_(port) {
        
        client_ = std::make_unique<httplib::SSLClient>(host, port);
        
        // Disabilita verifica certificato per test (NON fare in produzione!)
        client_->enable_server_certificate_verification(false);
        
        // Timeout
        client_->set_connection_timeout(5, 0);
        client_->set_read_timeout(5, 0);
        client_->set_write_timeout(5, 0);
    }
    
    void set_api_key(const std::string& api_key) {
        api_key_ = api_key;
    }
    
    void set_client_certificates(const std::string& ca_cert, 
                                const std::string& client_cert, 
                                const std::string& client_key) {
        ca_cert_ = ca_cert;
        client_cert_ = client_cert;
        client_key_ = client_key;
        
        // Nota: cpp-httplib potrebbe non supportare certificati client in tutte le versioni
        // Per il test, utilizziamo solo la CA per la verifica del server
        if (!ca_cert.empty()) {
            client_->set_ca_cert_path(ca_cert.c_str());
            client_->enable_server_certificate_verification(true);
            log("CA certificato configurato per verifica server");
        }
        
        if (!client_cert.empty() && !client_key.empty()) {
            log("⚠️  Nota: Certificati client specificati ma potrebbero non essere supportati dalla versione di cpp-httplib");
            log("    Per test completi con certificati client, usa curl o openssl s_client");
        }
    }
    
    bool test_health() {
        log("Testing health endpoint...");
        
        httplib::Headers headers;
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }
        
        auto res = client_->Get("/health", headers);
        
        if (res && res->status == 200) {
            auto data = json::parse(res->body);
            log("✓ Health check passed: " + data.dump());
            return true;
        } else {
            log("✗ Health check failed: " + (res ? std::to_string(res->status) : "Connection error"));
            return false;
        }
    }
    
    bool send_metrics(const json& metrics) {
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }
        
        auto res = client_->Post("/metrics", headers, metrics.dump(), "application/json");
        
        if (res && res->status == 200) {
            auto response = json::parse(res->body);
            log("✓ Metrics sent successfully: " + response.dump());
            return true;
        } else {
            log("✗ Failed to send metrics: " + (res ? std::to_string(res->status) : "Connection error"));
            if (res) {
                log("Response body: " + res->body);
            }
            return false;
        }
    }
    
    bool query_metrics(const std::string& name = "", size_t limit = 10) {
        log("Querying metrics...");
        
        httplib::Headers headers;
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }
        
        std::string path = "/metrics";
        if (!name.empty()) {
            path += "?name=" + name;
        }
        path += "&limit=" + std::to_string(limit);
        
        auto res = client_->Get(path.c_str(), headers);
        
        if (res && res->status == 200) {
            auto data = json::parse(res->body);
            log("✓ Query successful. Found " + std::to_string(data.size()) + " metrics");
            
            // Mostra alcune metriche
            for (size_t i = 0; i < std::min(data.size(), size_t(3)); ++i) {
                std::cout << "  - " << data[i]["name"] << " from " 
                         << data[i]["tags"]["host"] << std::endl;
            }
            
            return true;
        } else {
            log("✗ Query failed: " + (res ? std::to_string(res->status) : "Connection error"));
            return false;
        }
    }
    
    bool get_statistics(const std::string& metric_name, const std::string& field_name) {
        log("Getting statistics for " + metric_name + "." + field_name + "...");
        
        httplib::Headers headers;
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }
        
        std::string path = "/metrics/" + metric_name + "/stats?field=" + field_name;
        auto res = client_->Get(path.c_str(), headers);
        
        if (res && res->status == 200) {
            auto stats = json::parse(res->body);
            log("✓ Statistics retrieved:");
            std::cout << "  Count: " << stats.value("count", 0) << std::endl;
            std::cout << "  Min: " << stats.value("min", 0.0) << std::endl;
            std::cout << "  Max: " << stats.value("max", 0.0) << std::endl;
            std::cout << "  Mean: " << stats.value("mean", 0.0) << std::endl;
            std::cout << "  StdDev: " << stats.value("stddev", 0.0) << std::endl;
            return true;
        } else {
            log("✗ Failed to get statistics: " + (res ? std::to_string(res->status) : "Connection error"));
            return false;
        }
    }
    
    void run_simulation(int duration_seconds = 60, int interval_ms = 1000) {
        log("Starting metrics simulation for " + std::to_string(duration_seconds) + " seconds...");
        
        auto start_time = std::chrono::steady_clock::now();
        auto end_time = start_time + std::chrono::seconds(duration_seconds);
        
        std::vector<std::string> hostnames = {"server-01", "server-02", "server-03"};
        std::vector<std::string> apps = {"web-app", "api-service", "database"};
        std::vector<std::string> interfaces = {"eth0", "eth1"};
        
        int batch_count = 0;
        
        while (std::chrono::steady_clock::now() < end_time) {
            json metrics_batch = json::array();
            
            // Genera metriche per ogni host
            for (const auto& host : hostnames) {
                metrics_batch.push_back(generate_system_metrics(host));
            }
            
            // Genera metriche applicative
            for (const auto& app : apps) {
                metrics_batch.push_back(generate_app_metrics(app));
            }
            
            // Genera metriche di rete
            for (const auto& iface : interfaces) {
                metrics_batch.push_back(generate_network_metrics(iface));
            }
            
            // Invia batch
            if (send_metrics(metrics_batch)) {
                batch_count++;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        
        log("Simulation completed. Sent " + std::to_string(batch_count) + " batches");
    }
    
    void run_streaming_test(int duration_seconds = 30) {
        log("Testing streaming endpoint...");
        
        httplib::Headers headers;
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }
        
        // Test SSE streaming
        std::thread sse_thread([this, headers, duration_seconds]() {
            log("Connecting to SSE stream...");
            
            auto res = client_->Get("/stream?metrics=test_metric,system&tag.host=server-01", 
                headers,
                [this](const char* data, size_t data_length) {
                    std::string chunk(data, data_length);
                    
                    // Parse SSE events
                    std::istringstream stream(chunk);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (line.find("data: ") == 0) {
                            try {
                                auto metric_json = json::parse(line.substr(6));
                                log("📊 Received streaming metric: " + 
                                    metric_json["name"].get<std::string>() + 
                                    " from " + metric_json["tags"]["host"].get<std::string>());
                            } catch (...) {
                                // Ignora errori di parsing (potrebbero essere heartbeat)
                            }
                        }
                    }
                    return true;
                }
            );
            
            if (!res || res->status != 200) {
                log("✗ SSE streaming failed");
            }
        });
        
        // Invia alcune metriche mentre lo streaming è attivo
        std::this_thread::sleep_for(2s);
        
        for (int i = 0; i < 5; i++) {
            json test_metric = {
                {"name", "test_metric"},
                {"tags", {{"host", "server-01"}, {"test", "streaming"}}},
                {"fields", {{"value", 100.0 + i * 10}}}
            };
            
            send_metrics(json::array({test_metric}));
            std::this_thread::sleep_for(1s);
        }
        
        // Aspetta che il thread SSE termini
        sse_thread.join();
        
        log("Streaming test completed");
    }
    
    void test_compression() {
        log("Testing gzip compression...");
        
        // Genera un batch grande di metriche per testare la compressione
        json large_batch = json::array();
        for (int i = 0; i < 100; i++) {
            large_batch.push_back({
                {"name", "compression_test"},
                {"tags", {
                    {"host", "server-" + std::to_string(i % 10)},
                    {"datacenter", "dc-" + std::to_string(i % 3)},
                    {"environment", "production"}
                }},
                {"fields", {
                    {"value1", std::uniform_real_distribution<double>{0.0, 1000.0}(rng_)},
                    {"value2", std::uniform_real_distribution<double>{0.0, 1000.0}(rng_)},
                    {"value3", std::uniform_real_distribution<double>{0.0, 1000.0}(rng_)},
                    {"value4", std::uniform_real_distribution<double>{0.0, 1000.0}(rng_)},
                    {"value5", std::uniform_real_distribution<double>{0.0, 1000.0}(rng_)}
                }}
            });
        }
        
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("Accept-Encoding", "gzip");
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }
        
        std::string payload = large_batch.dump();
        log("Sending " + std::to_string(payload.size()) + " bytes (uncompressed)");
        
        auto res = client_->Post("/metrics", headers, payload, "application/json");
        
        if (res && res->status == 200) {
            log("✓ Compression test passed");
            
            // Verifica se la risposta è compressa
            if (res->has_header("Content-Encoding") && 
                res->get_header_value("Content-Encoding") == "gzip") {
                log("✓ Response was gzip compressed");
            }
        } else {
            log("✗ Compression test failed");
        }
    }
    
    void run_telegraf_format_test() {
        log("Testing Telegraf format endpoint...");
        
        // Formato Telegraf JSON
        json telegraf_data = {
            {"metrics", json::array({
                {
                    {"name", "cpu"},
                    {"tags", {
                        {"host", "telegraf-test"},
                        {"cpu", "cpu-total"}
                    }},
                    {"fields", {
                        {"usage_idle", 23.0},
                        {"usage_iowait", 0.5},
                        {"usage_system", 12.3},
                        {"usage_user", 64.2}
                    }},
                    {"timestamp", std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()}
                }
            })}
        };
        
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }
        
        auto res = client_->Post("/telegraf", headers, telegraf_data.dump(), "application/json");
        
        if (res && res->status == 204) {
            log("✓ Telegraf format test passed");
        } else {
            log("✗ Telegraf format test failed: " + (res ? std::to_string(res->status) : "Connection error"));
        }
    }
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --host <address>      Server address (default: localhost)\n"
              << "  --port <port>         Server port (default: 8443)\n"
              << "  --api-key <key>       API key for authentication\n"
              << "  --ca <file>           CA certificate file\n"
              << "  --cert <file>         Client certificate file\n"
              << "  --key <file>          Client private key file\n"
              << "  --test                Run basic tests\n"
              << "  --simulate <seconds>  Run simulation for N seconds\n"
              << "  --interval <ms>       Interval between metric sends (default: 1000)\n"
              << "  --telegraf            Test Telegraf format\n"
              << "  --streaming <seconds> Test streaming endpoint\n"
              << "  --compression         Test gzip compression\n"
              << "  --help                Show this help\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 8443;
    std::string api_key;
    std::string ca_cert, client_cert, client_key;
    bool run_tests = false;
    bool test_telegraf = false;
    bool test_compression = false;
    int simulate_duration = 0;
    int streaming_duration = 0;
    int interval_ms = 1000;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--api-key" && i + 1 < argc) {
            api_key = argv[++i];
        } else if (arg == "--ca" && i + 1 < argc) {
            ca_cert = argv[++i];
        } else if (arg == "--cert" && i + 1 < argc) {
            client_cert = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            client_key = argv[++i];
        } else if (arg == "--test") {
            run_tests = true;
        } else if (arg == "--simulate" && i + 1 < argc) {
            simulate_duration = std::stoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            interval_ms = std::stoi(argv[++i]);
        } else if (arg == "--telegraf") {
            test_telegraf = true;
        } else if (arg == "--streaming" && i + 1 < argc) {
            streaming_duration = std::stoi(argv[++i]);
        } else if (arg == "--compression") {
            test_compression = true;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    // Crea client
    MetricsTestClient client(host, port);
    
    if (!api_key.empty()) {
        client.set_api_key(api_key);
    }
    
    if (!ca_cert.empty() || !client_cert.empty()) {
        client.set_client_certificates(ca_cert, client_cert, client_key);
    }
    
    std::cout << "Connecting to https://" << host << ":" << port << "\n\n";
    
    // Esegui test richiesti
    if (run_tests) {
        std::cout << "=== Running Basic Tests ===\n";
        
        // Test health endpoint
        if (!client.test_health()) {
            std::cerr << "Health check failed. Is the server running?\n";
            return 1;
        }
        
        // Invia alcune metriche di test
        json test_metrics = json::array({
            {
                {"name", "test_metric"},
                {"tags", {{"test", "true"}}},
                {"fields", {{"value", 42.0}}}
            }
        });
        
        client.send_metrics(test_metrics);
        
        // Query delle metriche
        std::this_thread::sleep_for(100ms);
        client.query_metrics("test_metric");
        
        // Statistiche
        client.send_metrics(json::array({
            {{"name", "test_metric"}, {"fields", {{"value", 10.0}}}},
            {{"name", "test_metric"}, {"fields", {{"value", 20.0}}}},
            {{"name", "test_metric"}, {"fields", {{"value", 30.0}}}}
        }));
        
        std::this_thread::sleep_for(100ms);
        client.get_statistics("test_metric", "value");
        
        std::cout << "\n";
    }
    
    if (test_telegraf) {
        client.run_telegraf_format_test();
        std::cout << "\n";
    }
    
    if (test_compression) {
        client.test_compression();
        std::cout << "\n";
    }
    
    if (streaming_duration > 0) {
        client.run_streaming_test(streaming_duration);
        std::cout << "\n";
    }
    
    if (simulate_duration > 0) {
        client.run_simulation(simulate_duration, interval_ms);
    }
    
    if (!run_tests && !test_telegraf && !test_compression && 
        streaming_duration == 0 && simulate_duration == 0) {
        std::cout << "No action specified. Use --help for options.\n";
        return 1;
    }
    
    std::cout << "Test completed.\n";
    return 0;
}