// metrics_server.cpp
// Server REST in C++20 per acquisizione metriche da Telegraf con TLS

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <numeric>
#include <set>
#include <atomic>

// Librerie esterne necessarie (da installare)
#include <httplib.h> // cpp-httplib: https://github.com/yhirose/cpp-httplib
#include <nlohmann/json.hpp> // nlohmann/json: https://github.com/nlohmann/json

// OpenSSL headers per validazione certificati
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/pem.h>

using json = nlohmann::json;
using namespace std::chrono_literals;

// Struttura per rappresentare una metrica
struct Metric {
    std::string name;
    std::map<std::string, std::string> tags;
    std::map<std::string, double> fields;
    std::chrono::system_clock::time_point timestamp;
    
    json to_json() const {
        json j;
        j["name"] = name;
        j["tags"] = tags;
        j["fields"] = fields;
        j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()).count();
        return j;
    }
    
    static Metric from_json(const json& j) {
        Metric m;
        m.name = j["name"];
        m.tags = j["tags"].get<std::map<std::string, std::string>>();
        m.fields = j["fields"].get<std::map<std::string, double>>();
        
        if (j.contains("timestamp")) {
            auto ms = std::chrono::milliseconds(j["timestamp"].get<long long>());
            m.timestamp = std::chrono::system_clock::time_point(ms);
        } else {
            m.timestamp = std::chrono::system_clock::now();
        }
        
        return m;
    }
};

// Gestione client per streaming real-time
class StreamingClient {
public:
    using MetricCallback = std::function<void(const Metric&)>;
    
private:
    std::string id_;
    MetricCallback callback_;
    std::set<std::string> filters_; // Filtri per nome metrica
    std::map<std::string, std::string> tag_filters_; // Filtri per tags
    
public:
    StreamingClient(const std::string& id, MetricCallback callback) 
        : id_(id), callback_(callback) {}
    
    void set_filters(const std::set<std::string>& metric_names) {
        filters_ = metric_names;
    }
    
    void set_tag_filters(const std::map<std::string, std::string>& tags) {
        tag_filters_ = tags;
    }
    
    bool should_receive(const Metric& metric) const {
        // Se non ci sono filtri, ricevi tutto
        if (filters_.empty() && tag_filters_.empty()) return true;
        
        // Verifica filtro nome
        if (!filters_.empty() && filters_.find(metric.name) == filters_.end()) {
            return false;
        }
        
        // Verifica filtri tag
        for (const auto& [key, value] : tag_filters_) {
            auto it = metric.tags.find(key);
            if (it == metric.tags.end() || it->second != value) {
                return false;
            }
        }
        
        return true;
    }
    
    void send_metric(const Metric& metric) {
        if (should_receive(metric) && callback_) {
            callback_(metric);
        }
    }
    
    const std::string& id() const { return id_; }
};

// Storage thread-safe per le metriche con supporto streaming
class MetricsStorage {
private:
    mutable std::mutex mutex_;
    std::vector<Metric> metrics_;
    size_t max_metrics_ = 10000; // Limite massimo di metriche in memoria
    
    // Gestione streaming clients
    mutable std::mutex streaming_mutex_;
    std::map<std::string, std::shared_ptr<StreamingClient>> streaming_clients_;
    
public:
    void add_metric(const Metric& metric) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.push_back(metric);
            
            // Rimuovi le metriche più vecchie se superiamo il limite
            if (metrics_.size() > max_metrics_) {
                metrics_.erase(metrics_.begin(), metrics_.begin() + (metrics_.size() - max_metrics_));
            }
        }
        
        // Notifica i client streaming
        notify_streaming_clients(metric);
    }
    
    void add_metrics(const std::vector<Metric>& metrics) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.insert(metrics_.end(), metrics.begin(), metrics.end());
            
            if (metrics_.size() > max_metrics_) {
                metrics_.erase(metrics_.begin(), metrics_.begin() + (metrics_.size() - max_metrics_));
            }
        }
        
        // Notifica i client streaming per ogni metrica
        for (const auto& metric : metrics) {
            notify_streaming_clients(metric);
        }
    }
    
    std::vector<Metric> get_metrics(const std::string& name = "", 
                                   const std::map<std::string, std::string>& tags = {},
                                   size_t limit = 1000) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Metric> result;
        
        for (const auto& metric : metrics_) {
            bool match = true;
            
            // Filtra per nome se specificato
            if (!name.empty() && metric.name != name) {
                match = false;
            }
            
            // Filtra per tags se specificati
            for (const auto& [key, value] : tags) {
                auto it = metric.tags.find(key);
                if (it == metric.tags.end() || it->second != value) {
                    match = false;
                    break;
                }
            }
            
            if (match) {
                result.push_back(metric);
                if (result.size() >= limit) break;
            }
        }
        
        return result;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.clear();
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_.size();
    }
    
    // Calcola statistiche per una metrica specifica
    json get_statistics(const std::string& metric_name, const std::string& field_name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<double> values;
        
        for (const auto& metric : metrics_) {
            if (metric.name == metric_name) {
                auto it = metric.fields.find(field_name);
                if (it != metric.fields.end()) {
                    values.push_back(it->second);
                }
            }
        }
        
        json stats;
        if (!values.empty()) {
            std::sort(values.begin(), values.end());
            
            double sum = std::accumulate(values.begin(), values.end(), 0.0);
            double mean = sum / values.size();
            
            double sq_sum = std::inner_product(values.begin(), values.end(), values.begin(), 0.0);
            double stdev = std::sqrt(sq_sum / values.size() - mean * mean);
            
            stats["count"] = values.size();
            stats["min"] = values.front();
            stats["max"] = values.back();
            stats["mean"] = mean;
            stats["median"] = values[values.size() / 2];
            stats["stddev"] = stdev;
            stats["sum"] = sum;
        }
        
        return stats;
    }
    
    // Metodi per gestione streaming
    std::string add_streaming_client(StreamingClient::MetricCallback callback) {
        std::lock_guard<std::mutex> lock(streaming_mutex_);
        
        // Genera ID univoco
        auto now = std::chrono::system_clock::now();
        auto id = "client_" + std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count());
        
        streaming_clients_[id] = std::make_shared<StreamingClient>(id, callback);
        return id;
    }
    
    void remove_streaming_client(const std::string& client_id) {
        std::lock_guard<std::mutex> lock(streaming_mutex_);
        streaming_clients_.erase(client_id);
    }
    
    void set_client_filters(const std::string& client_id, 
                          const std::set<std::string>& metric_names,
                          const std::map<std::string, std::string>& tags = {}) {
        std::lock_guard<std::mutex> lock(streaming_mutex_);
        auto it = streaming_clients_.find(client_id);
        if (it != streaming_clients_.end()) {
            it->second->set_filters(metric_names);
            it->second->set_tag_filters(tags);
        }
    }
    
private:
    void notify_streaming_clients(const Metric& metric) {
        std::lock_guard<std::mutex> lock(streaming_mutex_);
        for (const auto& [id, client] : streaming_clients_) {
            // Notifica in modo asincrono per non bloccare
            std::thread([client, metric]() {
                client->send_metric(metric);
            }).detach();
        }
    }
};

// Configurazione del server
struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8443;
    std::string cert_file = "server.crt";
    std::string key_file = "server.key";
    std::string ca_file = "ca.crt"; // Per autenticazione client
    bool require_client_cert = true;
    std::string api_key = ""; // API key opzionale per autenticazione aggiuntiva
    bool enable_compression = true; // Abilita compressione gzip
    int compression_level = 6; // Livello compressione (1-9)
};

// Server REST per metriche
class MetricsRESTServer {
private:
    ServerConfig config_;
    std::unique_ptr<httplib::SSLServer> server_;
    MetricsStorage storage_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    X509* ca_cert_ = nullptr; // Certificato CA per validazione
    
    // Logger semplice
    void log(const std::string& level, const std::string& message) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::cout << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") 
                  << "] [" << level << "] " << message << std::endl;
    }
    
    // Carica certificato CA per validazione
    bool load_ca_certificate() {
        FILE* ca_file = fopen(config_.ca_file.c_str(), "r");
        if (!ca_file) {
            log("ERROR", "Failed to open CA file: " + config_.ca_file);
            return false;
        }
        
        ca_cert_ = PEM_read_X509(ca_file, nullptr, nullptr, nullptr);
        fclose(ca_file);
        
        if (!ca_cert_) {
            log("ERROR", "Failed to load CA certificate");
            return false;
        }
        
        return true;
    }
    
    // Callback per validazione certificato client
    static int verify_callback(int preverify_ok, X509_STORE_CTX* ctx) {
        if (!preverify_ok) {
            X509* cert = X509_STORE_CTX_get_current_cert(ctx);
            int err = X509_STORE_CTX_get_error(ctx);
            int depth = X509_STORE_CTX_get_error_depth(ctx);
            
            char subject[256];
            X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
            
            std::cerr << "Certificate verification failed at depth " << depth 
                     << ": " << X509_verify_cert_error_string(err)
                     << " for " << subject << std::endl;
        }
        
        return preverify_ok;
    }
    
    // Verifica autenticazione API key se configurata
    bool verify_api_key(const httplib::Request& req) {
        if (config_.api_key.empty()) return true;
        
        auto auth_header = req.get_header_value("Authorization");
        if (auth_header.empty()) return false;
        
        // Supporta formato "Bearer <api_key>"
        if (auth_header.find("Bearer ") == 0) {
            return auth_header.substr(7) == config_.api_key;
        }
        
        return auth_header == config_.api_key;
    }
    
    // Middleware per logging e autenticazione
    void setup_middleware() {
        server_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            log("INFO", "Request: " + req.method + " " + req.path + " from " + req.remote_addr);
            
            // Verifica API key se configurata
            if (!verify_api_key(req)) {
                res.status = 401;
                res.set_content(R"({"error": "Unauthorized"})", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            
            return httplib::Server::HandlerResponse::Unhandled;
        });
        
        // GET /stream - Server-Sent Events per streaming real-time
        server_->Get("/stream", [this](const httplib::Request& req, httplib::Response& res) {
            log("INFO", "New SSE connection from " + req.remote_addr);
            
            // Imposta headers per SSE
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no"); // Disabilita buffering proxy
            
            // Estrai filtri dalla query string
            std::set<std::string> metric_filters;
            std::map<std::string, std::string> tag_filters;
            
            if (req.has_param("metrics")) {
                std::string metrics_param = req.get_param_value("metrics");
                std::stringstream ss(metrics_param);
                std::string metric;
                while (std::getline(ss, metric, ',')) {
                    metric_filters.insert(metric);
                }
            }
            
            for (const auto& [key, value] : req.params) {
                if (key.find("tag.") == 0) {
                    tag_filters[key.substr(4)] = value;
                }
            }
            
            // Configurazione del content provider per streaming
            res.set_content_provider(
                "text/event-stream",
                [this, metric_filters, tag_filters](size_t offset, httplib::DataSink& sink) {
                    std::atomic<bool> client_connected{true};
                    std::condition_variable cv;
                    std::mutex cv_mutex;
                    std::queue<std::string> message_queue;
                    
                    // Registra client per ricevere metriche
                    auto client_id = storage_.add_streaming_client(
                        [&message_queue, &cv, &cv_mutex](const Metric& metric) {
                            std::lock_guard<std::mutex> lock(cv_mutex);
                            
                            // Formatta metrica come evento SSE
                            std::stringstream event;
                            event << "event: metric\n";
                            event << "data: " << metric.to_json().dump() << "\n\n";
                            
                            message_queue.push(event.str());
                            cv.notify_one();
                        }
                    );
                    
                    // Imposta filtri per il client
                    storage_.set_client_filters(client_id, metric_filters, tag_filters);
                    
                    // Invia heartbeat iniziale
                    sink.write(":ok\n\n", 5);
                    
                    // Thread per inviare heartbeat periodici
                    std::thread heartbeat_thread([&sink, &client_connected]() {
                        while (client_connected) {
                            std::this_thread::sleep_for(30s);
                            if (client_connected && !sink.write(":heartbeat\n\n", 13)) {
                                client_connected = false;
                                break;
                            }
                        }
                    });
                    
                    // Loop principale per inviare metriche
                    while (client_connected) {
                        std::unique_lock<std::mutex> lock(cv_mutex);
                        
                        // Attendi nuove metriche o timeout
                        if (cv.wait_for(lock, 5s, [&message_queue]() { 
                            return !message_queue.empty(); 
                        })) {
                            // Invia tutte le metriche in coda
                            while (!message_queue.empty() && client_connected) {
                                const auto& message = message_queue.front();
                                if (!sink.write(message.c_str(), message.length())) {
                                    client_connected = false;
                                    break;
                                }
                                message_queue.pop();
                            }
                        }
                    }
                    
                    // Cleanup
                    client_connected = false;
                    heartbeat_thread.join();
                    storage_.remove_streaming_client(client_id);
                    
                    log("INFO", "SSE connection closed");
                    return true;
                }
            );
        });
        
        // WebSocket endpoint alternativo usando long polling
        // GET /ws/metrics - Simula WebSocket con long polling
        server_->Get("/ws/metrics", [this](const httplib::Request& req, httplib::Response& res) {
            static std::map<std::string, std::chrono::system_clock::time_point> last_fetch;
            
            std::string client_id = req.get_param_value("client_id");
            if (client_id.empty()) {
                client_id = "anon_" + std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count());
            }
            
            auto since = last_fetch[client_id];
            last_fetch[client_id] = std::chrono::system_clock::now();
            
            // Ottieni metriche dal timestamp
            std::vector<Metric> new_metrics;
            auto all_metrics = storage_.get_metrics();
            
            for (const auto& metric : all_metrics) {
                if (metric.timestamp > since) {
                    new_metrics.push_back(metric);
                }
            }
            
            json response = {
                {"client_id", client_id},
                {"metrics", json::array()}
            };
            
            for (const auto& metric : new_metrics) {
                response["metrics"].push_back(metric.to_json());
            }
            
            res.set_content(response.dump(), "application/json");
        });
        
        // CORS headers
        server_->set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        });
    }
    
    // Setup degli endpoint REST
    void setup_endpoints() {
        // Health check
        server_->Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
            json response = {
                {"status", "healthy"},
                {"metrics_count", storage_.size()},
                {"timestamp", std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()}
            };
            res.set_content(response.dump(), "application/json");
        });
        
        // POST /metrics - Ricevi metriche da Telegraf
        server_->Post("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                json data = json::parse(req.body);
                std::vector<Metric> metrics;
                
                // Supporta sia singola metrica che array di metriche
                if (data.is_array()) {
                    for (const auto& item : data) {
                        metrics.push_back(Metric::from_json(item));
                    }
                } else {
                    metrics.push_back(Metric::from_json(data));
                }
                
                storage_.add_metrics(metrics);
                
                json response = {
                    {"status", "success"},
                    {"metrics_received", metrics.size()}
                };
                res.set_content(response.dump(), "application/json");
                
                log("INFO", "Received " + std::to_string(metrics.size()) + " metrics");
                
            } catch (const std::exception& e) {
                json error = {
                    {"error", "Invalid JSON"},
                    {"message", e.what()}
                };
                res.status = 400;
                res.set_content(error.dump(), "application/json");
                log("ERROR", "Failed to parse metrics: " + std::string(e.what()));
            }
        });
        
        // GET /metrics - Query metriche
        server_->Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
            std::string name = req.get_param_value("name");
            size_t limit = 1000;
            
            if (req.has_param("limit")) {
                try {
                    limit = std::stoull(req.get_param_value("limit"));
                } catch (...) {
                    limit = 1000;
                }
            }
            
            // Estrai tags dai parametri query
            std::map<std::string, std::string> tags;
            for (const auto& [key, value] : req.params) {
                if (key.find("tag.") == 0) {
                    tags[key.substr(4)] = value;
                }
            }
            
            auto metrics = storage_.get_metrics(name, tags, limit);
            
            json response = json::array();
            for (const auto& metric : metrics) {
                response.push_back(metric.to_json());
            }
            
            res.set_content(response.dump(), "application/json");
        });
        
        // GET /metrics/:name/stats - Statistiche per una metrica
        server_->Get(R"(/metrics/([^/]+)/stats)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string metric_name = req.matches[1];
            std::string field_name = req.get_param_value("field");
            
            if (field_name.empty()) {
                json error = {{"error", "Missing required parameter: field"}};
                res.status = 400;
                res.set_content(error.dump(), "application/json");
                return;
            }
            
            auto stats = storage_.get_statistics(metric_name, field_name);
            res.set_content(stats.dump(), "application/json");
        });
        
        // DELETE /metrics - Pulisci tutte le metriche
        server_->Delete("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
            storage_.clear();
            json response = {{"status", "success"}, {"message", "All metrics cleared"}};
            res.set_content(response.dump(), "application/json");
            log("INFO", "All metrics cleared");
        });
        
        // Endpoint per Telegraf output plugin
        server_->Post("/telegraf", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                // Telegraf invia metriche in formato Line Protocol o JSON
                // Qui assumiamo formato JSON
                json data = json::parse(req.body);
                std::vector<Metric> metrics;
                
                // Formato Telegraf JSON
                if (data.contains("metrics")) {
                    for (const auto& m : data["metrics"]) {
                        Metric metric;
                        metric.name = m["name"];
                        
                        if (m.contains("tags")) {
                            metric.tags = m["tags"].get<std::map<std::string, std::string>>();
                        }
                        
                        if (m.contains("fields")) {
                            for (const auto& [key, value] : m["fields"].items()) {
                                if (value.is_number()) {
                                    metric.fields[key] = value.get<double>();
                                }
                            }
                        }
                        
                        if (m.contains("timestamp")) {
                            metric.timestamp = std::chrono::system_clock::time_point(
                                std::chrono::seconds(m["timestamp"].get<long>()));
                        } else {
                            metric.timestamp = std::chrono::system_clock::now();
                        }
                        
                        metrics.push_back(metric);
                    }
                }
                
                storage_.add_metrics(metrics);
                res.status = 204; // No Content - successo per Telegraf
                
                log("INFO", "Received " + std::to_string(metrics.size()) + " metrics from Telegraf");
                
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(e.what(), "text/plain");
                log("ERROR", "Failed to parse Telegraf data: " + std::string(e.what()));
            }
        });
    }
    
public:
    MetricsRESTServer(const ServerConfig& config) : config_(config) {}
    
    bool start() {
        try {
            // Crea server HTTPS
            server_ = std::make_unique<httplib::SSLServer>(
                config_.cert_file.c_str(), 
                config_.key_file.c_str()
            );
            
            if (!server_->is_valid()) {
                log("ERROR", "Failed to initialize SSL server. Check certificate files.");
                return false;
            }
            
            // Carica certificato CA per validazione
            if (config_.require_client_cert && !config_.ca_file.empty()) {
                if (!load_ca_certificate()) {
                    return false;
                }
                
                SSL_CTX* ctx = server_->ssl_context();
                
                // Carica CA file nel contesto SSL
                if (SSL_CTX_load_verify_locations(ctx, config_.ca_file.c_str(), nullptr) != 1) {
                    log("ERROR", "Failed to load CA file: " + config_.ca_file);
                    return false;
                }
                
                // Configura validazione certificati client
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, 
                                  verify_callback);
                
                // Imposta profondità verifica catena certificati
                SSL_CTX_set_verify_depth(ctx, 5);
                
                // Carica CA nel trust store per validazione
                X509_STORE* store = SSL_CTX_get_cert_store(ctx);
                if (ca_cert_) {
                    X509_STORE_add_cert(store, ca_cert_);
                }
                
                log("INFO", "Client certificate validation enabled with CA: " + config_.ca_file);
            }
            
            // Configura compressione gzip se supportata
            if (config_.enable_compression) {
                // cpp-httplib supporta automaticamente gzip se compilato con CPPHTTPLIB_ZLIB_SUPPORT
                log("INFO", "Gzip compression enabled (automatic with CPPHTTPLIB_ZLIB_SUPPORT)");
            }
            
            setup_middleware();
            setup_endpoints();
            
            // Avvia server in thread separato
            running_ = true;
            server_thread_ = std::thread([this]() {
                log("INFO", "Starting HTTPS server on " + config_.host + ":" + std::to_string(config_.port));
                server_->listen(config_.host.c_str(), config_.port);
            });
            
            return true;
            
        } catch (const std::exception& e) {
            log("ERROR", "Failed to start server: " + std::string(e.what()));
            return false;
        }
    }
    
    void stop() {
        if (running_) {
            running_ = false;
            if (server_) {
                server_->stop();
            }
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
            log("INFO", "Server stopped");
        }
    }
    
    ~MetricsRESTServer() {
        stop();
        if (ca_cert_) {
            X509_free(ca_cert_);
        }
    }
};

// Funzione per generare certificati self-signed di esempio
void generate_example_certificates() {
    std::cout << "Generating example certificates..." << std::endl;
    
    // Script per generare certificati di esempio
    std::string script = R"(#!/bin/bash
# Genera CA
openssl genrsa -out ca.key 2048
openssl req -new -x509 -days 365 -key ca.key -out ca.crt -subj "/C=IT/ST=Lombardy/L=Milan/O=MetricsServer/CN=MetricsCA"

# Genera certificato server
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr -subj "/C=IT/ST=Lombardy/L=Milan/O=MetricsServer/CN=localhost"
openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt

# Genera certificato client per Telegraf
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr -subj "/C=IT/ST=Lombardy/L=Milan/O=MetricsServer/CN=telegraf-client"
openssl x509 -req -days 365 -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out client.crt

echo "Certificates generated successfully!"
)";
    
    std::ofstream script_file("generate_certs.sh");
    script_file << script;
    script_file.close();
    
    system("chmod +x generate_certs.sh && ./generate_certs.sh");
}

// Esempio di configurazione Telegraf
void print_telegraf_config() {
    std::cout << R"(
# Esempio di configurazione Telegraf per inviare metriche al server

[[outputs.http]]
  url = "https://localhost:8443/telegraf"
  method = "POST"
  data_format = "json"
  
  # Autenticazione TLS
  tls_ca = "/path/to/ca.crt"
  tls_cert = "/path/to/client.crt"
  tls_key = "/path/to/client.key"
  
  # Se usi API key
  # headers = {"Authorization" = "Bearer your-api-key"}
  
  # Timeout
  timeout = "5s"
  
  # Batch size
  metric_batch_size = 1000
  
  # Buffer
  metric_buffer_limit = 10000

# Esempio di input plugin
[[inputs.cpu]]
  percpu = true
  totalcpu = true
  collect_cpu_time = false
  report_active = false

[[inputs.mem]]
  # no configuration

[[inputs.disk]]
  ignore_fs = ["tmpfs", "devtmpfs", "devfs", "iso9660", "overlay", "aufs", "squashfs"]
)" << std::endl;
}

int main(int argc, char* argv[]) {
    // Configurazione di esempio
    ServerConfig config;
    
    // Parsing argomenti command line
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = std::stoi(argv[++i]);
        } else if (arg == "--cert" && i + 1 < argc) {
            config.cert_file = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            config.key_file = argv[++i];
        } else if (arg == "--ca" && i + 1 < argc) {
            config.ca_file = argv[++i];
        } else if (arg == "--api-key" && i + 1 < argc) {
            config.api_key = argv[++i];
        } else if (arg == "--no-client-cert") {
            config.require_client_cert = false;
        } else if (arg == "--no-compression") {
            config.enable_compression = false;
        } else if (arg == "--compression-level" && i + 1 < argc) {
            config.compression_level = std::stoi(argv[++i]);
        } else if (arg == "--generate-certs") {
            generate_example_certificates();
            return 0;
        } else if (arg == "--telegraf-config") {
            print_telegraf_config();
            return 0;
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --host <address>      Server address (default: 0.0.0.0)\n"
                      << "  --port <port>         Server port (default: 8443)\n"
                      << "  --cert <file>         Server certificate file\n"
                      << "  --key <file>          Server private key file\n"
                      << "  --ca <file>           CA certificate for client auth\n"
                      << "  --api-key <key>       API key for additional auth\n"
                      << "  --no-client-cert      Disable client certificate requirement\n"
                      << "  --no-compression      Disable gzip compression\n"
                      << "  --compression-level   Compression level 1-9 (default: 6)\n"
                      << "  --generate-certs      Generate example certificates\n"
                      << "  --telegraf-config     Print example Telegraf configuration\n"
                      << "  --help                Show this help\n";
            return 0;
        }
    }
    
    // Verifica che i certificati esistano
    std::ifstream cert_check(config.cert_file);
    std::ifstream key_check(config.key_file);
    if (!cert_check || !key_check) {
        std::cerr << "Certificate files not found. Run with --generate-certs to create examples.\n";
        return 1;
    }
    
    // Crea e avvia il server
    MetricsRESTServer server(config);
    
    if (!server.start()) {
        std::cerr << "Failed to start server\n";
        return 1;
    }
    
    std::cout << "Metrics REST Server started on https://" << config.host << ":" << config.port << "\n";
    std::cout << "Press Ctrl+C to stop...\n";
    
    // Gestione segnali per shutdown pulito
    std::condition_variable cv;
    std::mutex cv_m;
    std::unique_lock<std::mutex> lock(cv_m);
    
    std::signal(SIGINT, [](int) {
        std::cout << "\nShutting down...\n";
        exit(0);
    });
    
    cv.wait(lock);
    
    return 0;
}

/* 
Compilazione:
g++ -std=c++20 -o metrics_server metrics_server.cpp -lssl -lcrypto -pthread -lz

Dipendenze da installare:
- cpp-httplib: https://github.com/yhirose/cpp-httplib
- nlohmann/json: https://github.com/nlohmann/json

Su Ubuntu/Debian:
sudo apt-get install libssl-dev zlib1g-dev
sudo apt-get install nlohmann-json3-dev

Per cpp-httplib, scarica il singolo header:
wget https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h

Esempio di utilizzo:
1. Genera certificati: ./metrics_server --generate-certs
2. Avvia server: ./metrics_server --api-key mysecretkey
3. Configura Telegraf: ./metrics_server --telegraf-config > telegraf.conf
4. Test con curl:
   curl -k -X POST https://localhost:8443/metrics \
        -H "Authorization: Bearer mysecretkey" \
        -H "Content-Type: application/json" \
        -H "Accept-Encoding: gzip" \
        -d '[{"name":"cpu_usage","tags":{"host":"server1"},"fields":{"usage":45.5}}]'

5. Streaming real-time:
   curl -k -H "Authorization: Bearer mysecretkey" \
        https://localhost:8443/stream?metrics=cpu_usage,memory&tag.host=server1

6. Test compressione:
   curl -k -X GET https://localhost:8443/metrics \
        -H "Authorization: Bearer mysecretkey" \
        -H "Accept-Encoding: gzip" \
        --compressed
*/