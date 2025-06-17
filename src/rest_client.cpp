#include "rest_client.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>

RestClient::RestClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

RestClient::~RestClient() { 
    cleanup(); 
    curl_global_cleanup();
}

bool RestClient::initialize() { 
    curl_ = curl_easy_init();
    if (curl_) {
        // Set basic curl options
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, "CryptoHFTBot/1.0");
    }
    return curl_ != nullptr; 
}

void RestClient::cleanup() {
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
}

void RestClient::setApiCredentials(const std::string& api_key, const std::string& secret_key) {
    api_key_ = api_key;
    secret_key_ = secret_key;
}

void RestClient::setBaseUrl(const std::string& base_url) {
    base_url_ = base_url;
}

RestResponse RestClient::ping() {
    RestResponse response;
    response.success = true;
    response.http_code = 200;
    response.response_body = "{}";
    std::cout << "REST stub: Ping successful" << std::endl;
    return response;
}

RestResponse RestClient::getServerTime() {
    RestResponse response;
    response.success = true;
    response.http_code = 200;
    response.response_body = R"({"serverTime": 1640995200000})";
    return response;
}

RestResponse RestClient::getExchangeInfo() { 
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::getOrderBook(const std::string& symbol, int limit) { 
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::getTicker24hr(const std::string& symbol) { 
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::getAccountInfo() { 
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::getOpenOrders(const std::string& symbol) { 
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::getAllOrders(const std::string& symbol, const std::string& order_id) { 
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::placeOrder(const std::string& symbol, const std::string& side, 
                                   const std::string& type, const std::string& time_in_force,
                                   double quantity, double price, const std::string& client_order_id) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["side"] = side;
    params["type"] = type;
    params["timeInForce"] = time_in_force;
    
    // Format quantity and price properly
    std::ostringstream qty_oss, price_oss;
    qty_oss << std::fixed << std::setprecision(8) << quantity;
    price_oss << std::fixed << std::setprecision(2) << price;
    
    params["quantity"] = qty_oss.str();
    if (price > 0) {
        params["price"] = price_oss.str();
    }
    
    if (!client_order_id.empty()) {
        params["newClientOrderId"] = client_order_id;
    }
    
    auto response = post("/api/v3/order", params, true);
    
    if (response.success) {
        std::cout << "✅ ORDER PLACED: " << side << " " << quantity << " " << symbol 
                  << " @ $" << price << std::endl;
    } else {
        std::cout << "❌ ORDER FAILED: " << response.error_message << std::endl;
    }
    
    return response;
}

RestResponse RestClient::cancelOrder(const std::string& symbol, const std::string& order_id, 
                                    const std::string& client_order_id) {
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::getOrderStatus(const std::string& symbol, const std::string& order_id, 
                                       const std::string& client_order_id) {
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::cancelAllOpenOrders(const std::string& symbol) { 
    RestResponse response;
    response.success = true;
    return response;
}

BinanceApiLimits RestClient::getApiLimits() const { return api_limits_; }
bool RestClient::isRateLimited() const { return false; }
void RestClient::updateRateLimits(const std::map<std::string, std::string>& headers) {}

bool RestClient::isHealthy() const { return true; }
double RestClient::getAverageResponseTime() const { return 50.0; }

void RestClient::printStats() const {
    std::cout << "REST Stats - Total: " << total_requests_ 
              << " Success: " << successful_requests_ 
              << " Failed: " << failed_requests_ << std::endl;
}

// Private helper stubs
RestResponse RestClient::makeRequest(const std::string& method, const std::string& endpoint,
                                   const std::map<std::string, std::string>& params, bool requires_signature) {
    RestResponse response;
    total_requests_++;
    
    if (!curl_) {
        response.success = false;
        response.error_message = "CURL not initialized";
        failed_requests_++;
        return response;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // Build parameters with timestamp for signed requests
        std::map<std::string, std::string> all_params = params;
        if (requires_signature) {
            all_params["timestamp"] = std::to_string(getCurrentTimestamp());
        }
        
        // Build query string
        std::string query_string = buildQueryString(all_params);
        
        // Add signature for signed requests
        if (requires_signature && !secret_key_.empty()) {
            std::string signature = createSignature(query_string);
            query_string += "&signature=" + signature;
        }
        
        // Build full URL
        std::string url = base_url_ + endpoint;
        if (method == "GET" && !query_string.empty()) {
            url += "?" + query_string;
        }
        
        // Setup request
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response.response_body);
        
        // Setup headers
        struct curl_slist* headers = nullptr;
        if (!api_key_.empty()) {
            std::string api_key_header = "X-MBX-APIKEY: " + api_key_;
            headers = curl_slist_append(headers, api_key_header.c_str());
        }
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        
        // Setup method-specific options
        if (method == "POST") {
            curl_easy_setopt(curl_, CURLOPT_POST, 1L);
            if (!query_string.empty()) {
                curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, query_string.c_str());
            }
        } else if (method == "DELETE") {
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
            if (!query_string.empty()) {
                curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, query_string.c_str());
            }
        } else {
            curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        }
        
        // Perform request
        CURLcode curl_code = curl_easy_perform(curl_);
        
        // Get response code
        long http_code;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        response.http_code = static_cast<int>(http_code);
        
        // Cleanup headers
        curl_slist_free_all(headers);
        
        // Check for errors
        if (curl_code != CURLE_OK) {
            response.success = false;
            response.error_message = curl_easy_strerror(curl_code);
            failed_requests_++;
        } else if (http_code >= 400) {
            response.success = false;
            response.error_message = "HTTP error: " + std::to_string(http_code);
            failed_requests_++;
        } else {
            response.success = true;
            successful_requests_++;
        }
        
    } catch (const std::exception& e) {
        response.success = false;
        response.error_message = e.what();
        failed_requests_++;
    }
    
    // Update response time stats
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    response.response_time_ms = duration.count();
    
    return response;
}

RestResponse RestClient::get(const std::string& endpoint, const std::map<std::string, std::string>& params, bool requires_signature) {
    return makeRequest("GET", endpoint, params, requires_signature);
}

RestResponse RestClient::post(const std::string& endpoint, const std::map<std::string, std::string>& params, bool requires_signature) {
    return makeRequest("POST", endpoint, params, requires_signature);
}

RestResponse RestClient::delete_request(const std::string& endpoint, const std::map<std::string, std::string>& params, bool requires_signature) {
    return makeRequest("DELETE", endpoint, params, requires_signature);
}

std::string RestClient::createSignature(const std::string& query_string) const {
    return hmacSha256(secret_key_, query_string);
}

std::string RestClient::hmacSha256(const std::string& key, const std::string& data) const {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    
    HMAC(EVP_sha256(), key.c_str(), key.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         hash, &hash_len);
    
    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return oss.str();
}

std::string RestClient::buildQueryString(const std::map<std::string, std::string>& params) const {
    std::ostringstream oss;
    bool first = true;
    
    for (const auto& [key, value] : params) {
        if (!first) {
            oss << "&";
        }
        oss << key << "=" << value;
        first = false;
    }
    
    return oss.str();
}

uint64_t RestClient::getCurrentTimestamp() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

size_t RestClient::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
    size_t total_size = size * nmemb;
    response->append((char*)contents, total_size);
    return total_size;
}

void RestClient::setupCurlHeaders() {}
void RestClient::cleanupCurlHeaders() {}
bool RestClient::processResponse(const RestResponse& response, nlohmann::json& json_response) const { return true; }
void RestClient::updateStats(const RestResponse& response) {}
std::string RestClient::getCurlErrorString(CURLcode code) const { return ""; }
bool RestClient::shouldRetry(const RestResponse& response) const { return false; }

RestResponse RestClient::retryRequest(const std::string& method, const std::string& endpoint,
                                     const std::map<std::string, std::string>& params, bool requires_signature) {
    return makeRequest(method, endpoint, params, requires_signature);
}

bool RestClient::validateApiCredentials() const { return true; }
bool RestClient::validateParameters(const std::map<std::string, std::string>& params) const { return true; } 