// simple_ssl_client.cpp
// Client SSL semplice per test con certificati client

#include <iostream>
#include <string>
#include <sstream>
#include <cstring>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class SimpleSSLClient {
private:
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* bio_ = nullptr;
    std::string host_;
    int port_;
    
    void print_ssl_error(const std::string& msg) {
        std::cerr << msg << ": " << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    }
    
public:
    SimpleSSLClient(const std::string& host, int port) : host_(host), port_(port) {
        // Inizializza OpenSSL
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    }
    
    ~SimpleSSLClient() {
        cleanup();
    }
    
    bool connect(const std::string& ca_cert = "", 
                const std::string& client_cert = "", 
                const std::string& client_key = "") {
        
        // Crea contesto SSL
        const SSL_METHOD* method = TLS_client_method();
        ctx_ = SSL_CTX_new(method);
        if (!ctx_) {
            print_ssl_error("Failed to create SSL context");
            return false;
        }
        
        // Carica certificato CA se specificato
        if (!ca_cert.empty()) {
            if (SSL_CTX_load_verify_locations(ctx_, ca_cert.c_str(), nullptr) != 1) {
                print_ssl_error("Failed to load CA certificate");
                return false;
            }
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
        }
        
        // Carica certificato client se specificato
        if (!client_cert.empty() && !client_key.empty()) {
            if (SSL_CTX_use_certificate_file(ctx_, client_cert.c_str(), SSL_FILETYPE_PEM) != 1) {
                print_ssl_error("Failed to load client certificate");
                return false;
            }
            
            if (SSL_CTX_use_PrivateKey_file(ctx_, client_key.c_str(), SSL_FILETYPE_PEM) != 1) {
                print_ssl_error("Failed to load client key");
                return false;
            }
            
            if (SSL_CTX_check_private_key(ctx_) != 1) {
                print_ssl_error("Client certificate and key don't match");
                return false;
            }
        }
        
        // Crea BIO per connessione SSL
        bio_ = BIO_new_ssl_connect(ctx_);
        if (!bio_) {
            print_ssl_error("Failed to create BIO");
            return false;
        }
        
        // Ottieni SSL object
        BIO_get_ssl(bio_, &ssl_);
        if (!ssl_) {
            print_ssl_error("Failed to get SSL object");
            return false;
        }
        
        // Imposta hostname e porta
        std::string conn_str = host_ + ":" + std::to_string(port_);
        BIO_set_conn_hostname(bio_, conn_str.c_str());
        
        // Connetti
        if (BIO_do_connect(bio_) <= 0) {
            print_ssl_error("Failed to connect");
            return false;
        }
        
        // Verifica handshake SSL
        if (BIO_do_handshake(bio_) <= 0) {
            print_ssl_error("SSL handshake failed");
            return false;
        }
        
        std::cout << "✅ Connected successfully to " << host_ << ":" << port_ << std::endl;
        
        // Mostra info certificato server
        X509* cert = SSL_get_peer_certificate(ssl_);
        if (cert) {
            char* subject = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
            std::cout << "📜 Server certificate: " << subject << std::endl;
            OPENSSL_free(subject);
            X509_free(cert);
        }
        
        return true;
    }
    
    std::string send_request(const std::string& method, 
                           const std::string& path, 
                           const std::string& body = "",
                           const std::string& api_key = "") {
        if (!bio_) {
            std::cerr << "Not connected" << std::endl;
            return "";
        }
        
        // Costruisci richiesta HTTP
        std::stringstream request;
        request << method << " " << path << " HTTP/1.1\r\n";
        request << "Host: " << host_ << "\r\n";
        request << "Connection: close\r\n";
        
        if (!api_key.empty()) {
            request << "Authorization: Bearer " << api_key << "\r\n";
        }
        
        if (!body.empty()) {
            request << "Content-Type: application/json\r\n";
            request << "Content-Length: " << body.length() << "\r\n";
        }
        
        request << "\r\n";
        
        if (!body.empty()) {
            request << body;
        }
        
        // Invia richiesta
        std::string req_str = request.str();
        if (BIO_write(bio_, req_str.c_str(), req_str.length()) <= 0) {
            print_ssl_error("Failed to send request");
            return "";
        }
        
        // Leggi risposta
        std::string response;
        char buffer[4096];
        int bytes_read;
        
        while ((bytes_read = BIO_read(bio_, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            response += buffer;
        }
        
        return response;
    }
    
    void cleanup() {
        if (bio_) {
            BIO_free_all(bio_);
            bio_ = nullptr;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }
    
    // Estrai body JSON dalla risposta HTTP
    static json parse_response(const std::string& response) {
        size_t body_start = response.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            std::string body = response.substr(body_start + 4);
            try {
                return json::parse(body);
            } catch (const std::exception& e) {
                std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
            }
        }
        return json();
    }
};

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 8443;
    std::string ca_cert = "ca.crt";
    std::string client_cert = "client.crt";
    std::string client_key = "client.key";
    std::string api_key = "";
    
    // Parse argomenti
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--ca" && i + 1 < argc) {
            ca_cert = argv[++i];
        } else if (arg == "--cert" && i + 1 < argc) {
            client_cert = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            client_key = argv[++i];
        } else if (arg == "--api-key" && i + 1 < argc) {
            api_key = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --host <host>     Server host (default: localhost)\n"
                      << "  --port <port>     Server port (default: 8443)\n"
                      << "  --ca <file>       CA certificate file\n"
                      << "  --cert <file>     Client certificate file\n"
                      << "  --key <file>      Client key file\n"
                      << "  --api-key <key>   API key\n";
            return 0;
        }
    }
    
    std::cout << "🔐 Simple SSL Client Test\n" << std::endl;
    
    SimpleSSLClient client(host, port);
    
    // Connetti con certificati
    if (!client.connect(ca_cert, client_cert, client_key)) {
        std::cerr << "❌ Connection failed" << std::endl;
        return 1;
    }
    
    // Test 1: Health check
    std::cout << "\n📋 Test 1: Health Check" << std::endl;
    std::string response = client.send_request("GET", "/health", "", api_key);
    
    // Estrai status code
    size_t status_pos = response.find("HTTP/1.1 ");
    if (status_pos != std::string::npos) {
        std::string status_code = response.substr(status_pos + 9, 3);
        std::cout << "Status: " << status_code << std::endl;
    }
    
    json health_data = SimpleSSLClient::parse_response(response);
    if (!health_data.empty()) {
        std::cout << "Response: " << health_data.dump(2) << std::endl;
    }
    
    // Riconnetti per nuovo test
    client.cleanup();
    if (!client.connect(ca_cert, client_cert, client_key)) {
        return 1;
    }
    
    // Test 2: Invia metrica
    std::cout << "\n📋 Test 2: Send Metric" << std::endl;
    json metric = {
        {"name", "test_metric"},
        {"tags", {{"source", "ssl_client"}}},
        {"fields", {{"value", 123.45}}}
    };
    
    response = client.send_request("POST", "/metrics", metric.dump(), api_key);
    
    size_t status_pos2 = response.find("HTTP/1.1 ");
    if (status_pos2 != std::string::npos) {
        std::string status_code = response.substr(status_pos2 + 9, 3);
        std::cout << "Status: " << status_code << std::endl;
        
        if (status_code == "200") {
            std::cout << "✅ Metric sent successfully!" << std::endl;
        }
    }
    
    std::cout << "\n✅ All tests completed!" << std::endl;
    
    return 0;
}

/*
Compilazione:
g++ -std=c++20 -o simple_ssl_client simple_ssl_client.cpp -lssl -lcrypto

Uso:
./simple_ssl_client --host localhost --port 8443 --ca ca.crt \
    --cert client.crt --key client.key --api-key mysecretkey
*/