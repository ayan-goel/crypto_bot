#pragma once

#include <string>
#include <map>

class Config {
public:
    static Config& getInstance();

    bool loadFromFile(const std::string& filename = "config.txt");

    std::string getCoinbaseApiKey() const;
    std::string getCoinbaseSecretKey() const;
    std::string getCoinbaseWsUrl() const;

    double getOrderSize() const;
    double getMaxInventory() const;
    int getOrderRateLimit() const;

    double getTickSize() const;
    double getSpreadOffsetTicks() const;
    double getMinSpreadTicks() const;
    double getMaxNeutralPosition() const;
    double getInventoryCeiling() const;
    int getOrderLadderLevels() const;
    int getOrderEngineHz() const;

    std::string getConfig(const std::string& key, const std::string& default_val = "") const;

private:
    Config() = default;
    ~Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    std::map<std::string, std::string> config_map_;

    std::string getString(const std::string& key, const std::string& default_val = "") const;
    double getDouble(const std::string& key, double default_val = 0.0) const;
    int getInt(const std::string& key, int default_val = 0) const;

    static void trim(std::string& str);
};
