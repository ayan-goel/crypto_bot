#include "config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

bool Config::loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Could not open config file: " << filename << std::endl;
        return false;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;  // Skip empty lines and comments
        }
        
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            trim(key);
            trim(value);
            config_map_[key] = value;
        }
    }
    
    return true;
}

// Getters for configuration values
std::string Config::getBinanceApiKey() const {
    return getString("BINANCE_API_KEY");
}

std::string Config::getBinanceSecretKey() const {
    return getString("BINANCE_SECRET_KEY");
}

std::string Config::getBinanceBaseUrl() const {
    return getString("BINANCE_BASE_URL", "https://testnet.binance.vision");
}

std::string Config::getBinanceWsUrl() const {
    return getString("BINANCE_WS_URL", "wss://testnet.binance.vision/ws");
}

std::string Config::getTradingSymbol() const {
    return getString("TRADING_SYMBOL", "ETHUSDT");
}

std::string Config::getBaseAsset() const {
    return getString("BASE_ASSET", "ETH");
}

std::string Config::getQuoteAsset() const {
    return getString("QUOTE_ASSET", "USDT");
}

// Strategy parameters
double Config::getSpreadThresholdBps() const {
    return getDouble("SPREAD_THRESHOLD_BPS", 5.0);
}

double Config::getOrderSize() const {
    return getDouble("ORDER_SIZE", 0.01);
}

double Config::getMaxInventory() const {
    return getDouble("MAX_INVENTORY", 0.1);
}

int Config::getOrderRefreshIntervalMs() const {
    return getInt("ORDER_REFRESH_INTERVAL_MS", 200);
}

int Config::getOrderTimeoutSeconds() const {
    return getInt("ORDER_TIMEOUT_SECONDS", 30);
}

// Risk management
double Config::getMaxDailyDrawdown() const {
    return getDouble("MAX_DAILY_DRAWDOWN", 20.0);
}

double Config::getPositionLimit() const {
    return getDouble("POSITION_LIMIT", 0.1);
}

bool Config::isCircuitBreakerEnabled() const {
    return getBool("ENABLE_CIRCUIT_BREAKER", true);
}

// System configuration
std::string Config::getRedisHost() const {
    return getString("REDIS_HOST", "127.0.0.1");
}

int Config::getRedisPort() const {
    return getInt("REDIS_PORT", 6379);
}

int Config::getRedisDb() const {
    return getInt("REDIS_DB", 0);
}

// Logging
std::string Config::getLogLevel() const {
    return getString("LOG_LEVEL", "INFO");
}

bool Config::isLogToFile() const {
    return getBool("LOG_TO_FILE", true);
}

bool Config::isLogToConsole() const {
    return getBool("LOG_TO_CONSOLE", true);
}

// Performance settings
int Config::getOrderbookDepth() const {
    return getInt("ORDERBOOK_DEPTH", 10);
}

int Config::getWebsocketPingInterval() const {
    return getInt("WEBSOCKET_PING_INTERVAL", 30);
}

int Config::getRestTimeoutSeconds() const {
    return getInt("REST_TIMEOUT_SECONDS", 5);
}

int Config::getMaxReconnectAttempts() const {
    return getInt("MAX_RECONNECT_ATTEMPTS", 5);
}

// Development settings
bool Config::isTestnet() const {
    return getBool("USE_TESTNET", true);
}

bool Config::isPaperTrading() const {
    return getBool("PAPER_TRADING", true);
}

bool Config::isDebuggingEnabled() const {
    return getBool("ENABLE_DEBUGGING", false);
}

// Helper methods
std::string Config::getString(const std::string& key, const std::string& default_val) const {
    auto it = config_map_.find(key);
    return (it != config_map_.end()) ? it->second : default_val;
}

double Config::getDouble(const std::string& key, double default_val) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        try {
            return std::stod(it->second);
        } catch (const std::exception&) {
            // Fall through to default
        }
    }
    return default_val;
}

int Config::getInt(const std::string& key, int default_val) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        try {
            return std::stoi(it->second);
        } catch (const std::exception&) {
            // Fall through to default
        }
    }
    return default_val;
}

bool Config::getBool(const std::string& key, bool default_val) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        std::string value = it->second;
        // Remove leading and trailing whitespace inline
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), value.end());
        
        // Convert to lowercase for comparison
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return (value == "true" || value == "1" || value == "yes" || value == "on");
    }
    return default_val;
}

void Config::trim(std::string& str) {
    // Remove leading whitespace
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    
    // Remove trailing whitespace
    str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), str.end());
} 