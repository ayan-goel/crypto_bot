#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <chrono>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

struct RestResponse {
    bool success = false;
    int http_code = 0;
    std::string response_body;
    std::string error_message;
    double response_time_ms = 0.0;
};

struct CoinbaseApiLimits {
    int requests_per_second = 0;
    int burst_limit = 0;
    std::chrono::system_clock::time_point last_update;
};

class RestClient {
public:
    RestClient();
    ~RestClient();
    
    // Initialization
    bool initialize();
    void cleanup();
    
    // Authentication setup
    void setApiCredentials(const std::string& api_key, const std::string& secret_key, const std::string& passphrase = "");
    void setBaseUrl(const std::string& base_url);
    
    // Market Data (Public endpoints)
    RestResponse ping();
    RestResponse getServerTime();
    RestResponse getExchangeInfo();
    RestResponse getOrderBook(const std::string& symbol, int limit = 100);
    RestResponse getTicker24hr(const std::string& symbol);
    
    // Account & Trading (Private endpoints)
    RestResponse getAccountInfo();
    RestResponse getOpenOrders(const std::string& symbol = "");
    RestResponse getAllOrders(const std::string& symbol, const std::string& order_id = "");
    
    // Order operations
    RestResponse placeOrder(const std::string& symbol, 
                           const std::string& side, 
                           const std::string& type,
                           const std::string& time_in_force,
                           double quantity,
                           double price = 0.0,
                           const std::string& client_order_id = "");
    
    RestResponse cancelOrder(const std::string& symbol, 
                            const std::string& order_id = "",
                            const std::string& client_order_id = "");
    
    RestResponse getOrderStatus(const std::string& symbol, 
                               const std::string& order_id = "",
                               const std::string& client_order_id = "");
    
    // Batch operations
    RestResponse cancelAllOpenOrders(const std::string& symbol);
    
    // Rate limiting
    CoinbaseApiLimits getApiLimits() const;
    bool isRateLimited() const;
    void updateRateLimits(const std::map<std::string, std::string>& headers);
    
    // Health monitoring
    bool isHealthy() const;
    double getAverageResponseTime() const;
    void printStats() const;

private:
    // CURL handle
    CURL* curl_ = nullptr;
    struct curl_slist* headers_ = nullptr;
    
    // Authentication
    std::string api_key_;
    std::string secret_key_;
    std::string passphrase_;
    std::string base_url_;
    
    // Rate limiting
    CoinbaseApiLimits api_limits_;
    mutable std::mutex rate_limit_mutex_;
    
    // Statistics
    uint64_t total_requests_ = 0;
    uint64_t successful_requests_ = 0;
    uint64_t failed_requests_ = 0;
    double total_response_time_ = 0.0;
    mutable std::mutex stats_mutex_;
    
    // Configuration
    int timeout_seconds_ = 10;
    int max_retries_ = 3;
    int retry_delay_ms_ = 100;
    
    // Helper methods for HTTP requests
    RestResponse makeRequest(const std::string& method, 
                           const std::string& endpoint,
                           const std::map<std::string, std::string>& params = {},
                           bool requires_signature = false);
    
    RestResponse get(const std::string& endpoint, 
                    const std::map<std::string, std::string>& params = {},
                    bool requires_signature = false);
    
    RestResponse post(const std::string& endpoint,
                     const std::map<std::string, std::string>& params = {},
                     bool requires_signature = false);
    
    RestResponse postJson(const std::string& endpoint,
                         const std::string& json_body,
                         bool requires_signature = false);
    
    RestResponse delete_request(const std::string& endpoint,
                               const std::map<std::string, std::string>& params = {},
                               bool requires_signature = false);
    
    // Authentication helpers
    std::string createJwtToken(const std::string& method, const std::string& request_path, const std::string& body = "") const;
    std::string createSignature(const std::string& query_string) const;
    std::string hmacSha256(const std::string& key, const std::string& data) const;
    std::string base64Decode(const std::string& input) const;
    std::string buildQueryString(const std::map<std::string, std::string>& params) const;
    uint64_t getCurrentTimestamp() const;
    
    // CURL helpers
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response);
    void setupCurlHeaders();
    void cleanupCurlHeaders();
    
    // Response processing
    bool processResponse(const RestResponse& response, nlohmann::json& json_response) const;
    void updateStats(const RestResponse& response);
    
    // Error handling
    std::string getCurlErrorString(CURLcode code) const;
    bool shouldRetry(const RestResponse& response) const;
    RestResponse retryRequest(const std::string& method, 
                             const std::string& endpoint,
                             const std::map<std::string, std::string>& params,
                             bool requires_signature);
    
    // Validation
    bool validateApiCredentials() const;
    bool validateParameters(const std::map<std::string, std::string>& params) const;
    
    std::string hexEncode(const std::string& input) const;
}; 