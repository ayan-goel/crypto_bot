#pragma once

#include <string>
#include <map>
#include <memory>

class Config {
public:
    static Config& getInstance();
    
    // Load configuration from file
    bool loadFromFile(const std::string& filename = "config.txt");
    
    // Exchange API credentials (for market data)
    std::string getExchangeApiKey() const;
    std::string getExchangeSecretKey() const;
    std::string getExchangePassphrase() const;
    std::string getExchangeWsUrl() const;
    
    // Advanced Trade API credentials (for order execution)
    std::string getAdvancedTradeApiKey() const;
    std::string getAdvancedTradeSecretKey() const;
    std::string getAdvancedTradeWsUrl() const;
    
    // Legacy getters for configuration values
    std::string getCoinbaseApiKey() const;
    std::string getCoinbaseSecretKey() const;
    std::string getCoinbasePassphrase() const;
    std::string getCoinbaseBaseUrl() const;
    std::string getCoinbaseWsUrl() const;
    std::string getTradingSymbol() const;
    std::string getBaseAsset() const;
    std::string getQuoteAsset() const;
    
    // Strategy parameters
    double getSpreadThresholdBps() const;
    double getOrderSize() const;
    double getMaxInventory() const;
    int getOrderRefreshIntervalMs() const;
    int getOrderTimeoutSeconds() const;
    
    // Risk management
    double getMaxDailyDrawdown() const;
    double getMaxDailyLossLimit() const;
    double getPositionLimit() const;
    int getOrderRateLimit() const;
    bool isCircuitBreakerEnabled() const;
    
    // System configuration
    std::string getRedisHost() const;
    int getRedisPort() const;
    int getRedisDb() const;
    
    // Logging
    std::string getLogLevel() const;
    bool isLogToFile() const;
    bool isLogToConsole() const;
    
    // Performance settings
    int getOrderbookDepth() const;
    int getWebsocketPingInterval() const;
    int getRestTimeoutSeconds() const;
    int getMaxReconnectAttempts() const;
    
    // Development settings
    bool isTestnet() const;
    bool isPaperTrading() const;
    bool isDebuggingEnabled() const;
    
    // Generic config getter
    std::string getConfig(const std::string& key, const std::string& default_val = "") const;

private:
    Config() = default;
    ~Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    
    std::map<std::string, std::string> config_map_;
    
    // Helper methods
    std::string getString(const std::string& key, const std::string& default_val = "") const;
    double getDouble(const std::string& key, double default_val = 0.0) const;
    int getInt(const std::string& key, int default_val = 0) const;
    bool getBool(const std::string& key, bool default_val = false) const;

    
    void trim(std::string& str);
}; 