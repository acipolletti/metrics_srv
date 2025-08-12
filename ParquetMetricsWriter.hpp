#ifndef PARQUET_METRICS_WRITER_HPP
#define PARQUET_METRICS_WRITER_HPP

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/ipc/writer.h>
#include <arrow/compute/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/arrow/reader.h>
#include <parquet/properties.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <queue>
#include <thread>
#include <condition_variable>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>

namespace fs = std::filesystem;
using json = nlohmann::json;

class ParquetMetricsWriter {
public:
    struct Config {
        std::string base_path = "./metrics_data";
        size_t batch_size = 10000;
        size_t time_window_seconds = 3600;
        size_t max_file_size_mb = 100;
        bool enable_compression = true;
        parquet::Compression::type compression_type = parquet::Compression::SNAPPY;
        bool partition_by_date = true;
        bool partition_by_metric = false;
        bool partition_by_host = false;
        size_t retention_days = 30;
        size_t writer_threads = 2;
        bool enable_statistics = true;
        bool enable_dictionary = false;  // DISABLED by default to avoid issues
    };

    struct Statistics {
        std::atomic<size_t> total_records_written{0};
        std::atomic<size_t> total_files_created{0};
        std::atomic<size_t> total_bytes_written{0};
        std::atomic<size_t> write_errors{0};
        std::atomic<size_t> current_buffer_size{0};
        std::atomic<size_t> current_queue_size{0};
        std::atomic<size_t> records_dropped{0};
        std::chrono::steady_clock::time_point start_time;
        
        json toJson() const {
            auto now = std::chrono::steady_clock::now();
            auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - start_time).count();
            
            return {
                {"total_records_written", total_records_written.load()},
                {"total_files_created", total_files_created.load()},
                {"total_bytes_written", total_bytes_written.load()},
                {"total_mb_written", total_bytes_written.load() / (1024.0 * 1024.0)},
                {"write_errors", write_errors.load()},
                {"current_buffer_size", current_buffer_size.load()},
                {"current_queue_size", current_queue_size.load()},
                {"records_dropped", records_dropped.load()},
                {"uptime_seconds", uptime_seconds},
                {"records_per_second", uptime_seconds > 0 ? 
                    total_records_written.load() / static_cast<double>(uptime_seconds) : 0}
            };
        }
    };

    // Add query methods if needed
    arrow::Result<std::shared_ptr<arrow::Table>> queryMetrics(
        const std::string& file_path,
        const std::string& metric_filter = "",
        int64_t start_time = 0,
        int64_t end_time = std::numeric_limits<int64_t>::max()) {
        
        std::shared_ptr<arrow::io::RandomAccessFile> input;
        ARROW_ASSIGN_OR_RAISE(input, arrow::io::ReadableFile::Open(file_path));
        
        std::unique_ptr<parquet::arrow::FileReader> reader;
        auto reader_result = parquet::arrow::OpenFile(input, arrow::default_memory_pool());
        if (!reader_result.ok()) {
            return reader_result.status();
        }
        reader = std::move(reader_result.ValueOrDie());
        
        std::shared_ptr<arrow::Table> table;
        ARROW_RETURN_NOT_OK(reader->ReadTable(&table));
        
        return table;
    }
    
    size_t cleanupOldFiles() {
        auto cutoff = std::chrono::system_clock::now() - 
                     std::chrono::hours(config_.retention_days * 24);
        
        size_t deleted = 0;
        
        for (const auto& entry : fs::recursive_directory_iterator(config_.base_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".parquet") {
                auto ftime = fs::last_write_time(entry.path());
                auto ftime_sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                
                if (ftime_sys < cutoff) {
                    fs::remove(entry.path());
                    deleted++;
                }
            }
        }
        
        return deleted;
    }
    
private:
    Config config_;
    Statistics stats_;
    
    std::shared_ptr<arrow::Schema> schema_;
    
    struct MetricRecord {
        int64_t timestamp;
        std::string metric_name;
        double value;
        std::string host;
        std::map<std::string, std::string> tags;
        
        static constexpr size_t estimated_size() { return 256; }
    };
    
    std::vector<MetricRecord> buffer_active_;
    std::vector<MetricRecord> buffer_standby_;
    std::mutex buffer_mutex_;
    std::atomic<bool> use_active_buffer_{true};
    
    struct WriteBatch {
        std::vector<MetricRecord> records;
        std::chrono::system_clock::time_point timestamp;
    };
    
    std::queue<WriteBatch> write_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    std::vector<std::thread> writer_threads_;
    std::atomic<bool> should_stop_{false};
    
    std::map<std::string, std::chrono::system_clock::time_point> active_files_;
    std::mutex files_mutex_;
    
    std::thread cleanup_thread_;
    std::chrono::seconds cleanup_interval_{3600};

public:
    explicit ParquetMetricsWriter() : ParquetMetricsWriter(Config{}) {}
    
    explicit ParquetMetricsWriter(const Config& config) 
        : config_(config) {
        
        // IMPORTANT: Force disable dictionary encoding to avoid issues
        config_.enable_dictionary = false;
        
        stats_.start_time = std::chrono::steady_clock::now();
        
        buffer_active_.reserve(config_.batch_size);
        buffer_standby_.reserve(config_.batch_size);
        
        initializeSchema();
        createDirectories();
        startWriterThreads();
        startCleanupThread();
        
        std::cout << "[ParquetWriter] Initialized" << std::endl;
        std::cout << "  Path: " << config_.base_path << std::endl;
        std::cout << "  Batch: " << config_.batch_size << std::endl;
        std::cout << "  Compression: " << (config_.enable_compression ? "ON" : "OFF") << std::endl;
        std::cout << "  Dictionary: " << (config_.enable_dictionary ? "ON" : "OFF (recommended)") << std::endl;
        std::cout << "  Threads: " << config_.writer_threads << std::endl;
    }
    
    ~ParquetMetricsWriter() {
        flush();
        should_stop_ = true;
        queue_cv_.notify_all();
        
        for (auto& t : writer_threads_) {
            if (t.joinable()) t.join();
        }
        
        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }
        
        processRemainingQueue();
        
        std::cout << "[ParquetWriter] Stats: " << stats_.toJson().dump() << std::endl;
    }
    
    void addMetrics(const json& telegraf_data) {
        try {
            if (!telegraf_data.contains("metrics") || !telegraf_data["metrics"].is_array()) {
                return;
            }
            
            std::vector<MetricRecord> records;
            records.reserve(telegraf_data["metrics"].size());
            
            for (const auto& metric : telegraf_data["metrics"]) {
                processMetricJson(metric, records);
            }
            
            if (!records.empty()) {
                addRecordsToBatch(std::move(records));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[ParquetWriter] Error: " << e.what() << std::endl;
            stats_.write_errors++;
        }
    }
    
    void flush() {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        flushBufferLocked();
    }
    
    const Statistics& getStatistics() const {
        return stats_;
    }
    
    json getStats() const {
        return stats_.toJson();
    }

private:
    void initializeSchema() {
        // Create simple schema WITHOUT dictionary encoding
        std::vector<std::shared_ptr<arrow::Field>> fields = {
            arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO)),
            arrow::field("metric_name", arrow::utf8()),  // Simple string
            arrow::field("value", arrow::float64()),
            arrow::field("host", arrow::utf8()),         // Simple string
            arrow::field("tags_json", arrow::utf8())     // Simple string
        };
        
        schema_ = arrow::schema(fields);
        
        std::cout << "[ParquetWriter] Schema created (no dictionary encoding)" << std::endl;
    }
    
    void createDirectories() {
        fs::create_directories(config_.base_path);
        
        if (config_.partition_by_date) {
            auto today_path = generateDatePath(std::chrono::system_clock::now());
            fs::create_directories(today_path);
        }
    }
    
    void startWriterThreads() {
        for (size_t i = 0; i < config_.writer_threads; ++i) {
            writer_threads_.emplace_back([this, i]() {
                writerThreadLoop(i);
            });
        }
    }
    
    void startCleanupThread() {
        cleanup_thread_ = std::thread([this]() {
            while (!should_stop_) {
                std::this_thread::sleep_for(cleanup_interval_);
                if (!should_stop_) {
                    cleanupOldFiles();
                }
            }
        });
    }
    
    void processMetricJson(const json& metric, std::vector<MetricRecord>& records) {
        MetricRecord base_record;
        
        base_record.timestamp = metric.value("timestamp", 0) * 1000000000LL;
        base_record.metric_name = metric.value("name", "unknown");
        
        if (metric.contains("tags") && metric["tags"].is_object()) {
            base_record.host = metric["tags"].value("host", "unknown");
            
            for (auto& [key, value] : metric["tags"].items()) {
                if (value.is_string()) {
                    base_record.tags[key] = value.get<std::string>();
                } else {
                    base_record.tags[key] = value.dump();
                }
            }
        } else {
            base_record.host = "unknown";
        }
        
        if (metric.contains("value")) {
            base_record.value = metric["value"].get<double>();
            records.push_back(base_record);
            
        } else if (metric.contains("fields") && metric["fields"].is_object()) {
            for (auto& [field_name, field_value] : metric["fields"].items()) {
                if (field_value.is_number()) {
                    MetricRecord field_record = base_record;
                    field_record.metric_name += "." + field_name;
                    field_record.value = field_value.get<double>();
                    records.push_back(field_record);
                }
            }
        }
    }
    
    void addRecordsToBatch(std::vector<MetricRecord>&& records) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        
        auto& buffer = use_active_buffer_ ? buffer_active_ : buffer_standby_;
        
        for (auto& record : records) {
            buffer.push_back(std::move(record));
        }
        
        stats_.current_buffer_size = buffer.size();
        
        if (buffer.size() >= config_.batch_size) {
            flushBufferLocked();
        }
    }
    
    void flushBufferLocked() {
        auto& buffer = use_active_buffer_ ? buffer_active_ : buffer_standby_;
        
        if (buffer.empty()) return;
        
        WriteBatch batch;
        batch.records = std::move(buffer);
        batch.timestamp = std::chrono::system_clock::now();
        buffer.clear();
        
        use_active_buffer_ = !use_active_buffer_;
        stats_.current_buffer_size = 0;
        
        {
            std::lock_guard<std::mutex> qlock(queue_mutex_);
            
            if (write_queue_.size() >= 1000) {
                stats_.records_dropped += batch.records.size();
                return;
            }
            
            write_queue_.push(std::move(batch));
            stats_.current_queue_size = write_queue_.size();
        }
        
        queue_cv_.notify_one();
    }
    
    void writerThreadLoop(size_t thread_id) {
        while (!should_stop_) {
            WriteBatch batch;
            
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() {
                    return !write_queue_.empty() || should_stop_;
                });
                
                if (should_stop_ && write_queue_.empty()) break;
                
                if (!write_queue_.empty()) {
                    batch = std::move(write_queue_.front());
                    write_queue_.pop();
                    stats_.current_queue_size = write_queue_.size();
                }
            }
            
            if (!batch.records.empty()) {
                writeToParquet(batch, thread_id);
            }
        }
    }
    
    void writeToParquet(const WriteBatch& batch, size_t thread_id) {
        try {
            std::string file_path = generateFilePath(batch.timestamp);
            
            // Create simple builders (NO dictionary builders)
            auto timestamp_builder = std::make_shared<arrow::TimestampBuilder>(
                arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
            auto name_builder = std::make_shared<arrow::StringBuilder>();
            auto value_builder = std::make_shared<arrow::DoubleBuilder>();
            auto host_builder = std::make_shared<arrow::StringBuilder>();
            auto tags_builder = std::make_shared<arrow::StringBuilder>();
            
            // Append data
            for (const auto& record : batch.records) {
                auto status = timestamp_builder->Append(record.timestamp);
                if (!status.ok()) throw std::runtime_error("Timestamp append failed");
                
                status = name_builder->Append(record.metric_name);
                if (!status.ok()) throw std::runtime_error("Name append failed");
                
                status = value_builder->Append(record.value);
                if (!status.ok()) throw std::runtime_error("Value append failed");
                
                status = host_builder->Append(record.host);
                if (!status.ok()) throw std::runtime_error("Host append failed");
                
                json tags_json = record.tags;
                status = tags_builder->Append(tags_json.dump());
                if (!status.ok()) throw std::runtime_error("Tags append failed");
            }
            
            // Finish arrays
            std::shared_ptr<arrow::Array> timestamp_array, name_array, 
                                          value_array, host_array, tags_array;
            
            auto status = timestamp_builder->Finish(&timestamp_array);
            if (!status.ok()) throw std::runtime_error("Timestamp finish failed");
            
            status = name_builder->Finish(&name_array);
            if (!status.ok()) throw std::runtime_error("Name finish failed");
            
            status = value_builder->Finish(&value_array);
            if (!status.ok()) throw std::runtime_error("Value finish failed");
            
            status = host_builder->Finish(&host_array);
            if (!status.ok()) throw std::runtime_error("Host finish failed");
            
            status = tags_builder->Finish(&tags_array);
            if (!status.ok()) throw std::runtime_error("Tags finish failed");
            
            // Create table
            auto table = arrow::Table::Make(schema_, 
                {timestamp_array, name_array, value_array, host_array, tags_array});
            
            // Configure writer - DISABLE dictionary encoding
            parquet::WriterProperties::Builder props_builder;
            
            if (config_.enable_compression) {
                props_builder.compression(config_.compression_type);
            }
            
            // IMPORTANT: Explicitly disable dictionary encoding
            props_builder.disable_dictionary();
            
            auto writer_props = props_builder.build();
            auto arrow_props = parquet::ArrowWriterProperties::Builder().build();
            
            // Write to file
            std::shared_ptr<arrow::io::FileOutputStream> outfile;
            auto result = arrow::io::FileOutputStream::Open(file_path);
            if (!result.ok()) {
                throw std::runtime_error("Failed to open file: " + file_path);
            }
            outfile = result.ValueOrDie();
            
            status = parquet::arrow::WriteTable(
                *table,
                arrow::default_memory_pool(),
                outfile,
                table->num_rows(),
                writer_props,
                arrow_props
            );
            
            if (!status.ok()) {
                throw std::runtime_error("Write table failed: " + status.ToString());
            }
            
            status = outfile->Close();
            if (!status.ok()) {
                throw std::runtime_error("Close file failed");
            }
            
            // Update stats
            size_t file_size = fs::file_size(file_path);
            stats_.total_records_written += batch.records.size();
            stats_.total_bytes_written += file_size;
            stats_.total_files_created++;
            
            std::cout << "[ParquetWriter] Thread " << thread_id 
                     << " wrote " << batch.records.size() 
                     << " records (" << file_size / 1024 << " KB)" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "[ParquetWriter] Thread " << thread_id 
                     << " ERROR: " << e.what() << std::endl;
            stats_.write_errors++;
        }
    }
    
    void processRemainingQueue() {
        while (!write_queue_.empty()) {
            WriteBatch batch;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (!write_queue_.empty()) {
                    batch = std::move(write_queue_.front());
                    write_queue_.pop();
                }
            }
            
            if (!batch.records.empty()) {
                writeToParquet(batch, 999);
            }
        }
    }
    
    std::string generateDatePath(const std::chrono::system_clock::time_point& time) {
        auto time_t = std::chrono::system_clock::to_time_t(time);
        
        std::stringstream ss;
        ss << config_.base_path;
        
        if (config_.partition_by_date) {
            ss << "/" << std::put_time(std::gmtime(&time_t), "%Y/%m/%d");
        }
        
        return ss.str();
    }
    
    std::string generateFilePath(const std::chrono::system_clock::time_point& time) {
        auto time_t = std::chrono::system_clock::to_time_t(time);
        
        std::stringstream ss;
        ss << generateDatePath(time);
        
        fs::create_directories(ss.str());
        
        ss << "/metrics_" 
           << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S")
           << "_" << std::chrono::duration_cast<std::chrono::milliseconds>(
                        time.time_since_epoch()).count() % 1000
           << ".parquet";
        
        return ss.str();
    }
    
    
    
};

#endif // PARQUET_METRICS_WRITER_HPP