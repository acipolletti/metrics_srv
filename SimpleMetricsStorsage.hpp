#ifndef SIMPLE_METRICS_STORAGE_HPP
#define SIMPLE_METRICS_STORAGE_HPP

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <thread>
#include <queue>
#include <atomic>
#include <zlib.h>  // For gzip compression
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Simple, lightweight metrics storage without external dependencies
class SimpleMetricsStorage {
public:
    enum class Format {
        JSONL,      // JSON Lines (one JSON per line)
        CSV,        // Comma-separated values
        JSONL_GZ    // Compressed JSON Lines
    };
    
    struct Config {
        std::string base_path = "./metrics_data";
        Format format = Format::JSONL_GZ;
        size_t batch_size = 1000;
        size_t max_file_size_mb = 100;
        size_t time_window_minutes = 60;
        bool partition_by_date = true;
        size_t retention_days = 30;
        size_t writer_threads = 2;
    };
    
    struct Stats {
        std::atomic<size_t> total_records{0};
        std::atomic<size_t> total_files{0};
        std::atomic<size_t> total_bytes{0};
        std::atomic<size_t> records_in_buffer{0};
        
        json toJson() const {
            return {
                {"total_records", total_records.load()},
                {"total_files", total_files.load()},
                {"total_mb", total_bytes.load() / (1024.0 * 1024.0)},
                {"buffer_size", records_in_buffer.load()}
            };
        }
    };

private:
    Config config_;
    Stats stats_;
    
    // Simple record structure
    struct Record {
        int64_t timestamp;
        std::string metric;
        double value;
        std::string host;
        std::string tags_json;
    };
    
    // Buffer
    std::vector<Record> buffer_;
    std::mutex buffer_mutex_;
    
    // Writer thread
    std::thread writer_thread_;
    std::atomic<bool> should_stop_{false};
    std::condition_variable write_cv_;
    
    // Current file
    std::string current_file_;
    std::ofstream current_stream_;
    std::mutex file_mutex_;
    size_t current_file_size_ = 0;
    std::chrono::system_clock::time_point current_file_time_;

public:
    explicit SimpleMetricsStorage(const Config& config = {}) 
        : config_(config) {
        
        fs::create_directories(config_.base_path);
        buffer_.reserve(config_.batch_size);
        
        // Start writer thread
        writer_thread_ = std::thread([this]() { writerLoop(); });
        
        std::cout << "[SimpleStorage] Initialized" << std::endl;
        std::cout << "  Path: " << config_.base_path << std::endl;
        std::cout << "  Format: " << formatToString(config_.format) << std::endl;
    }
    
    ~SimpleMetricsStorage() {
        flush();
        should_stop_ = true;
        write_cv_.notify_all();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
        closeCurrentFile();
    }
    
    // Add metrics from Telegraf JSON
    void addMetrics(const json& data) {
        if (!data.contains("metrics") || !data["metrics"].is_array()) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        
        for (const auto& metric : data["metrics"]) {
            // Extract base fields
            Record rec;
            rec.timestamp = metric.value("timestamp", 0);
            rec.metric = metric.value("name", "unknown");
            
            // Extract host
            if (metric.contains("tags") && metric["tags"].is_object()) {
                rec.host = metric["tags"].value("host", "unknown");
                rec.tags_json = metric["tags"].dump();
            } else {
                rec.host = "unknown";
                rec.tags_json = "{}";
            }
            
            // Handle different formats
            if (metric.contains("value")) {
                rec.value = metric["value"].get<double>();
                buffer_.push_back(rec);
                
            } else if (metric.contains("fields")) {
                // Multiple fields
                for (auto& [field, val] : metric["fields"].items()) {
                    if (val.is_number()) {
                        Record field_rec = rec;
                        field_rec.metric += "." + field;
                        field_rec.value = val.get<double>();
                        buffer_.push_back(field_rec);
                    }
                }
            }
        }
        
        stats_.records_in_buffer = buffer_.size();
        
        // Trigger write if buffer is full
        if (buffer_.size() >= config_.batch_size) {
            write_cv_.notify_one();
        }
    }
    
    // Force flush
    void flush() {
        write_cv_.notify_one();
    }
    
    // Get statistics
    Stats getStats() const { return stats_; }
    
    // Query functions for reading data back
    std::vector<json> query(const std::string& metric_filter = "",
                            int64_t start_time = 0,
                            int64_t end_time = std::numeric_limits<int64_t>::max()) {
        std::vector<json> results;
        
        // Find relevant files
        for (const auto& entry : fs::recursive_directory_iterator(config_.base_path)) {
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().string();
                if (ext == getFileExtension()) {
                    readFile(entry.path(), results, metric_filter, start_time, end_time);
                }
            }
        }
        
        return results;
    }
    
    // Cleanup old files
    size_t cleanupOldFiles() {
        auto cutoff = std::chrono::system_clock::now() - 
                     std::chrono::hours(config_.retention_days * 24);
        size_t deleted = 0;
        
        for (const auto& entry : fs::recursive_directory_iterator(config_.base_path)) {
            if (entry.is_regular_file()) {
                auto ftime = fs::last_write_time(entry.path());
                auto sctp = decltype(cutoff)::clock::now() - 
                           decltype(ftime)::clock::now() + cutoff;
                
                if (sctp < cutoff) {
                    fs::remove(entry.path());
                    deleted++;
                }
            }
        }
        
        return deleted;
    }

private:
    void writerLoop() {
        while (!should_stop_) {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            write_cv_.wait_for(lock, std::chrono::seconds(10), [this]() {
                return !buffer_.empty() || should_stop_;
            });
            
            if (should_stop_ && buffer_.empty()) break;
            
            if (!buffer_.empty()) {
                auto to_write = std::move(buffer_);
                buffer_.clear();
                stats_.records_in_buffer = 0;
                lock.unlock();
                
                writeRecords(to_write);
            }
        }
    }
    
    void writeRecords(const std::vector<Record>& records) {
        std::lock_guard<std::mutex> lock(file_mutex_);
        
        // Check if we need a new file
        if (shouldRotateFile()) {
            closeCurrentFile();
            openNewFile();
        }
        
        // Write records based on format
        switch (config_.format) {
            case Format::JSONL:
            case Format::JSONL_GZ:
                writeJsonLines(records);
                break;
            case Format::CSV:
                writeCsv(records);
                break;
        }
        
        stats_.total_records += records.size();
        current_file_size_ = current_stream_.tellp();
    }
    
    void writeJsonLines(const std::vector<Record>& records) {
        for (const auto& rec : records) {
            json j = {
                {"timestamp", rec.timestamp},
                {"metric", rec.metric},
                {"value", rec.value},
                {"host", rec.host},
                {"tags", json::parse(rec.tags_json)}
            };
            current_stream_ << j.dump() << "\n";
        }
        current_stream_.flush();
    }
    
    void writeCsv(const std::vector<Record>& records) {
        // Write header if new file
        if (current_file_size_ == 0) {
            current_stream_ << "timestamp,metric,value,host,tags\n";
        }
        
        for (const auto& rec : records) {
            current_stream_ << rec.timestamp << ","
                          << rec.metric << ","
                          << rec.value << ","
                          << rec.host << ","
                          << "\"" << rec.tags_json << "\"\n";
        }
        current_stream_.flush();
    }
    
    bool shouldRotateFile() {
        if (current_file_.empty()) return true;
        
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            now - current_file_time_).count();
        
        return (current_file_size_ >= config_.max_file_size_mb * 1024 * 1024) ||
               (elapsed >= config_.time_window_minutes);
    }
    
    void openNewFile() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        std::stringstream path;
        path << config_.base_path;
        
        if (config_.partition_by_date) {
            path << "/" << std::put_time(std::gmtime(&time_t), "%Y/%m/%d");
            fs::create_directories(path.str());
        }
        
        path << "/metrics_" 
             << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S")
             << getFileExtension();
        
        current_file_ = path.str();
        current_file_time_ = now;
        current_file_size_ = 0;
        
        if (config_.format == Format::JSONL_GZ) {
            // For simplicity, using regular stream
            // In production, use zlib or boost::iostreams for gzip
            current_stream_.open(current_file_, std::ios::out | std::ios::binary);
        } else {
            current_stream_.open(current_file_);
        }
        
        stats_.total_files++;
        std::cout << "[SimpleStorage] New file: " << current_file_ << std::endl;
    }
    
    void closeCurrentFile() {
        if (current_stream_.is_open()) {
            current_stream_.close();
            
            // If gzip format, compress the file
            if (config_.format == Format::JSONL_GZ && !current_file_.empty()) {
                compressFile(current_file_);
            }
            
            if (!current_file_.empty()) {
                stats_.total_bytes += fs::file_size(current_file_);
            }
        }
    }
    
    void compressFile(const std::string& filepath) {
        // Simple gzip compression using zlib
        std::ifstream input(filepath, std::ios::binary);
        std::ofstream output(filepath + ".gz", std::ios::binary);
        
        // Simple compression (in production, use proper streaming)
        std::string data((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
        
        z_stream stream;
        stream.zalloc = Z_NULL;
        stream.zfree = Z_NULL;
        stream.opaque = Z_NULL;
        
        if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                        15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
            
            stream.next_in = (Bytef*)data.data();
            stream.avail_in = data.size();
            
            std::vector<char> compressed(data.size());
            stream.next_out = (Bytef*)compressed.data();
            stream.avail_out = compressed.size();
            
            deflate(&stream, Z_FINISH);
            output.write(compressed.data(), stream.total_out);
            deflateEnd(&stream);
        }
        
        input.close();
        output.close();
        
        // Remove uncompressed file
        fs::remove(filepath);
        current_file_ += ".gz";
    }
    
    void readFile(const fs::path& path, std::vector<json>& results,
                  const std::string& metric_filter, int64_t start_time, int64_t end_time) {
        // Simple file reading (implement based on format)
        std::ifstream file(path);
        std::string line;
        
        while (std::getline(file, line)) {
            try {
                json j = json::parse(line);
                
                // Apply filters
                if (!metric_filter.empty() && j["metric"] != metric_filter) continue;
                if (j["timestamp"] < start_time || j["timestamp"] > end_time) continue;
                
                results.push_back(j);
            } catch (...) {
                // Skip invalid lines
            }
        }
    }
    
    std::string getFileExtension() const {
        switch (config_.format) {
            case Format::JSONL: return ".jsonl";
            case Format::CSV: return ".csv";
            case Format::JSONL_GZ: return ".jsonl.gz";
        }
        return ".dat";
    }
    
    std::string formatToString(Format fmt) const {
        switch (fmt) {
            case Format::JSONL: return "JSON Lines";
            case Format::CSV: return "CSV";
            case Format::JSONL_GZ: return "Compressed JSON Lines";
        }
        return "Unknown";
    }
};

#endif // SIMPLE_METRICS_STORAGE_HPP