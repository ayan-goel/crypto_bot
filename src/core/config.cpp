#include "core/config.h"
#include <fstream>
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
            continue;
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

std::string Config::getCoinbaseApiKey() const {
    return getString("COINBASE_API_KEY");
}

std::string Config::getCoinbaseSecretKey() const {
    return getString("COINBASE_SECRET_KEY");
}

std::string Config::getCoinbaseWsUrl() const {
    return getString("COINBASE_WS_URL", "wss://advanced-trade-ws.coinbase.com");
}

double Config::getOrderSize() const {
    return getDouble("ORDER_SIZE", 0.01);
}

double Config::getMaxInventory() const {
    return getDouble("MAX_INVENTORY", 0.1);
}

int Config::getOrderRateLimit() const {
    return getInt("ORDER_RATE_LIMIT", 100);
}

double Config::getTickSize() const {
    return getDouble("TICK_SIZE", 0.01);
}

double Config::getSpreadOffsetTicks() const {
    return getDouble("SPREAD_OFFSET_TICKS", 0.25);
}

double Config::getMinSpreadTicks() const {
    return getDouble("MIN_SPREAD_TICKS", 0.5);
}

double Config::getMaxNeutralPosition() const {
    return getDouble("MAX_NEUTRAL_POSITION", 0.01);
}

double Config::getInventoryCeiling() const {
    return getDouble("INVENTORY_CEILING", 0.02);
}

int Config::getOrderLadderLevels() const {
    return getInt("ORDER_LADDER_LEVELS", 5);
}

int Config::getOrderEngineHz() const {
    return getInt("ORDER_ENGINE_HZ", 2000);
}

std::string Config::getConfig(const std::string& key, const std::string& default_val) const {
    return getString(key, default_val);
}

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
            std::cerr << "Warning: invalid double for config key '" << key << "'" << std::endl;
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
            std::cerr << "Warning: invalid int for config key '" << key << "'" << std::endl;
        }
    }
    return default_val;
}

void Config::trim(std::string& str) {
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), str.end());
}
