#ifndef CSV_METRICS_WRITER_HPP
#define CSV_METRICS_WRITER_HPP

#include <fstream>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

class CSVMetricsWriter {
private:
    std::string base_path_ = "./metrics_data";
    std::ofstream current_file_;
    std::string current_filename_;
    std::mutex write_mutex_;
    size_t current_records_ = 0;
    size_t max_records_per_file_ = 100000;
    std::atomic<size_t> total_records_{0};
    std::atomic<size_t> total_files_{0};
    
public:
    CSVMetricsWriter() {
        fs::create_directories(base_path_);
        rotateFile();
    }
    
    ~CSVMetricsWriter() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (current_file_.is_open()) {
            current_file_.close();
        }
    }
    
    void addMetrics(const json& data) {
        if (!data.contains("metrics") || !data["metrics"].is_array()) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(write_mutex_);
        
        for (const auto& metric : data["metrics"]) {
            writeMetric(metric);
        }
        
        current_file_.flush();
    }
    
    json getStats() const {
        return {
            {"total_records", total_records_.load()},
            {"total_files", total_files_.load()},
            {"current_file", current_filename_},
            {"format", "CSV"}
        };
    }
    
private:
    void writeMetric(const json& metric) {
        // Check if need new file
        if (current_records_ >= max_records_per_file_) {
            rotateFile();
        }
        
        // Extract fields
        int64_t timestamp = metric.value("timestamp", 0);
        std::string name = metric.value("name", "unknown");
        std::string host = "unknown";
        
        if (metric.contains("tags") && metric["tags"].is_object()) {
            host = metric["tags"].value("host", "unknown");
        }
        
        // Write based on metric format
        if (metric.contains("value")) {
            // Single value
            double value = metric["value"].get<double>();
            current_file_ << timestamp << ","
                         << name << ","
                         << value << ","
                         << host << "\n";
            current_records_++;
            total_records_++;
            
        } else if (metric.contains("fields") && metric["fields"].is_object()) {
            // Multiple fields
            for (auto& [field, val] : metric["fields"].items()) {
                if (val.is_number()) {
                    current_file_ << timestamp << ","
                                 << name << "." << field << ","
                                 << val.get<double>() << ","
                                 << host << "\n";
                    current_records_++;
                    total_records_++;
                }
            }
        }
    }
    
    void rotateFile() {
        if (current_file_.is_open()) {
            current_file_.close();
        }
        
        // Generate new filename
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        std::stringstream ss;
        ss << base_path_ << "/";
        
        // Create date directories
        ss << std::put_time(std::gmtime(&time_t), "%Y/%m/%d");
        fs::create_directories(ss.str());
        
        ss << "/metrics_"
           << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S")
           << ".csv";
        
        current_filename_ = ss.str();
        current_file_.open(current_filename_);
        
        // Write CSV header
        current_file_ << "timestamp,metric,value,host\n";
        
        current_records_ = 0;
        total_files_++;
        
        std::cout << "[CSVWriter] New file: " << current_filename_ << std::endl;
    }
};

#endif // CSV_METRICS_WRITER_HPP