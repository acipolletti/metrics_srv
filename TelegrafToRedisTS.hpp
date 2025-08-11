#ifndef TelegrafToRedisTS_H
#define TelegrafToRedisTS_H
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <unordered_map>
#include <concepts>
#include <format>
#include <ranges>
#include <algorithm>
#include <iostream>  // Per l'esempio d'uso

// Concept per verificare che un tipo sia convertibile in stringa
template<typename T>
concept StringConvertible = requires(T t) {
    { std::to_string(t) } -> std::convertible_to<std::string>;
} || std::convertible_to<T, std::string>;

class RedisTimeSeriesGenerator {
public:
    // Struttura per rappresentare un comando Redis
    struct RedisCommand {
        std::string command;
        std::vector<std::string> args;
        
        // Converte il comando in stringa formattata per Redis
        [[nodiscard]] std::string toString() const {
            std::stringstream ss;
            ss << command;
            for (const auto& arg : args) {
                ss << " " << arg;
            }
            return ss.str();
        }
        
        // Converte in formato RESP (Redis Serialization Protocol)
        [[nodiscard]] std::string toRESP() const {
            std::stringstream ss;
            ss << "*" << (args.size() + 1) << "\r\n";
            ss << "$" << command.length() << "\r\n" << command << "\r\n";
            for (const auto& arg : args) {
                ss << "$" << arg.length() << "\r\n" << arg << "\r\n";
            }
            return ss.str();
        }
    };
    
    // Configurazione per la generazione dei comandi
    struct Config {
        bool createTimeSeries = true;              // Se creare automaticamente le time series
        bool useMultiAdd = true;                   // Se usare TS.MADD per batch di metriche
        std::optional<int64_t> retention = 86400;  // Retention in secondi (default 24h)
        bool duplicatePolicy = true;               // Se impostare duplicate policy
        std::string duplicatePolicyValue = "LAST"; // FIRST, LAST, MIN, MAX, SUM
        bool compressData = true;                  // Se abilitare la compressione
        std::optional<std::string> keyPrefix;      // Prefisso per le chiavi
        
        // Costruttore di default esplicito
        Config() = default;
        
        // Costruttore di copia
        Config(const Config&) = default;
        
        // Costruttore di move
        Config(Config&&) = default;
        
        // Operatore di assegnazione
        Config& operator=(const Config&) = default;
        Config& operator=(Config&&) = default;
    };

private:
    Config config_;
    std::unordered_map<std::string, bool> createdKeys_;
    
    // Genera la chiave per una metrica basata su nome e labels
    [[nodiscard]] std::string generateKey(const std::string& metric, 
                                          const nlohmann::json& labels = {}) const {
        std::stringstream ss;
        
        if (config_.keyPrefix.has_value()) {
            ss << config_.keyPrefix.value() << ":";
        }
        
        ss << metric;
        
        if (!labels.empty() && labels.is_object()) {
            // Ordina le labels per consistenza della chiave
            std::vector<std::pair<std::string, std::string>> sortedLabels;
            for (auto& [key, value] : labels.items()) {
                sortedLabels.emplace_back(key, value.get<std::string>());
            }
            std::ranges::sort(sortedLabels);
            
            for (const auto& [key, value] : sortedLabels) {
                ss << ":" << key << "=" << value;
            }
        }
        
        return ss.str();
    }
    
    // Genera il comando TS.CREATE
    [[nodiscard]] RedisCommand generateCreateCommand(const std::string& key,
                                                     const nlohmann::json& labels = {}) const {
        RedisCommand cmd{"TS.CREATE", {key}};
        
        if (config_.retention.has_value()) {
            cmd.args.push_back("RETENTION");
            cmd.args.push_back(std::to_string(config_.retention.value()));
        }
        
        if (config_.duplicatePolicy) {
            cmd.args.push_back("DUPLICATE_POLICY");
            cmd.args.push_back(config_.duplicatePolicyValue);
        }
        
        if (config_.compressData) {
            cmd.args.push_back("COMPRESSED");
        }
        
        // Aggiungi labels come metadata
        if (!labels.empty() && labels.is_object()) {
            cmd.args.push_back("LABELS");
            for (auto& [lkey, lvalue] : labels.items()) {
                cmd.args.push_back(lkey);
                cmd.args.push_back(lvalue.get<std::string>());
            }
        }
        
        return cmd;
    }
    
    // Genera il comando TS.ADD
    [[nodiscard]] RedisCommand generateAddCommand(const std::string& key,
                                                  int64_t timestamp,
                                                  double value) const {
        return RedisCommand{
            "TS.ADD",
            {key, std::to_string(timestamp), std::to_string(value)}
        };
    }
    
    // Processa una singola metrica JSON
    [[nodiscard]] std::vector<RedisCommand> processMetric(const nlohmann::json& metric) {
        std::vector<RedisCommand> commands;
        
        // Estrai i campi necessari
        std::string metricName = metric.value("metric", "unknown");
        int64_t timestamp = metric.value("timestamp", 0);
        double value = metric.value("value", 0.0);
        nlohmann::json labels = metric.value("labels", nlohmann::json::object());
        
        // Genera la chiave
        std::string key = generateKey(metricName, labels);
        
        // Crea la time series se necessario
        if (config_.createTimeSeries && createdKeys_.find(key) == createdKeys_.end()) {
            commands.push_back(generateCreateCommand(key, labels));
            createdKeys_[key] = true;
        }
        
        // Aggiungi il punto dati
        commands.push_back(generateAddCommand(key, timestamp, value));
        
        return commands;
    }

public:
    explicit RedisTimeSeriesGenerator(Config config) : config_(config) {}
    
    // Genera comandi da un singolo oggetto JSON
    [[nodiscard]] std::vector<RedisCommand> generateCommands(const nlohmann::json& input) {
        std::vector<RedisCommand> allCommands;
        
        // Se è un array, processa ogni elemento
        if (input.is_array()) {
            if (config_.useMultiAdd && input.size() > 1) {
                // Raggruppa per timestamp per usare TS.MADD
                std::unordered_map<int64_t, std::vector<std::pair<std::string, double>>> batchedData;
                
                for (const auto& item : input) {
                    int64_t timestamp = item.value("timestamp", 0);
                    std::string metricName = item.value("metric", "unknown");
                    double value = item.value("value", 0.0);
                    nlohmann::json labels = item.value("labels", nlohmann::json::object());
                    
                    std::string key = generateKey(metricName, labels);
                    
                    // Crea la time series se necessario
                    if (config_.createTimeSeries && createdKeys_.find(key) == createdKeys_.end()) {
                        allCommands.push_back(generateCreateCommand(key, labels));
                        createdKeys_[key] = true;
                    }
                    
                    batchedData[timestamp].emplace_back(key, value);
                }
                
                // Genera comandi TS.MADD per ogni timestamp
                for (const auto& [ts, metrics] : batchedData) {
                    RedisCommand maddCmd{"TS.MADD", {}};
                    for (const auto& [key, val] : metrics) {
                        maddCmd.args.push_back(key);
                        maddCmd.args.push_back(std::to_string(ts));
                        maddCmd.args.push_back(std::to_string(val));
                    }
                    allCommands.push_back(maddCmd);
                }
            } else {
                // Processa singolarmente
                for (const auto& item : input) {
                    auto cmds = processMetric(item);
                    allCommands.insert(allCommands.end(), cmds.begin(), cmds.end());
                }
            }
        } else if (input.is_object()) {
            // Processa singolo oggetto
            auto cmds = processMetric(input);
            allCommands.insert(allCommands.end(), cmds.begin(), cmds.end());
        }
        
        return allCommands;
    }
    
    // Genera comandi da una stringa JSONL (JSON Lines)
    [[nodiscard]] std::vector<RedisCommand> generateCommandsFromJSONL(const std::string& jsonl) {
        std::vector<RedisCommand> allCommands;
        std::istringstream stream(jsonl);
        std::string line;
        
        while (std::getline(stream, line)) {
            if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos) {
                continue;
            }
            
            try {
                auto json = nlohmann::json::parse(line);
                auto cmds = processMetric(json);
                allCommands.insert(allCommands.end(), cmds.begin(), cmds.end());
            } catch (const nlohmann::json::exception& e) {
                // Log or handle parse error
                // Per ora ignoriamo le righe non valide
            }
        }
        
        return allCommands;
    }
    
    // Resetta lo stato delle chiavi create (utile per test o reinizializzazione)
    void resetCreatedKeys() {
        createdKeys_.clear();
    }
    
    // Getters e setters per la configurazione
    [[nodiscard]] const Config& getConfig() const { return config_; }
    void setConfig(const Config& config) { config_ = config; }
};

// Esempio d'uso
inline void exampleUsage() {
    // Configurazione personalizzata
    RedisTimeSeriesGenerator::Config config;
    config.createTimeSeries = true;
    config.retention = 3600 * 24 * 7;  // 7 giorni
    config.keyPrefix = "metrics";
    config.duplicatePolicyValue = "LAST";
    
    // Crea il generatore
    RedisTimeSeriesGenerator generator(config);
    
    // JSON di esempio
    nlohmann::json metrics = nlohmann::json::parse(R"([
        {
            "timestamp": 1704067200,
            "metric": "cpu_usage",
            "value": 45.2,
            "labels": {
                "host": "server01",
                "region": "eu-west-1"
            }
        },
        {
            "timestamp": 1704067260,
            "metric": "memory_usage",
            "value": 78.5,
            "labels": {
                "host": "server01",
                "region": "eu-west-1"
            }
        }
    ])");
    
    // Genera i comandi
    auto commands = generator.generateCommands(metrics);
    
    // Stampa i comandi
    for (const auto& cmd : commands) {
        std::cout << cmd.toString() << std::endl;
    }
    
    // Oppure in formato RESP per invio diretto a Redis
    for (const auto& cmd : commands) {
        std::cout << cmd.toRESP();
    }
}
#endif