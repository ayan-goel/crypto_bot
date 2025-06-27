#include "rest_client.h"
#include "config.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <algorithm>
#include <vector>
#include <jwt-cpp/jwt.h>

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
        
        // Load Advanced Trade API credentials for order execution
        Config& config = Config::getInstance();
        std::string advanced_api_key = config.getAdvancedTradeApiKey();
        std::string advanced_secret = config.getAdvancedTradeSecretKey();
        std::string base_url = config.getCoinbaseBaseUrl();
        
        std::cout << "🔐 REST Client - Advanced Trade API Setup:" << std::endl;
        std::cout << "   API Key: " << (advanced_api_key.empty() ? "NOT SET" : "SET (" + std::to_string(advanced_api_key.length()) + " chars)") << std::endl;
        std::cout << "   Secret Key: " << (advanced_secret.empty() ? "NOT SET" : "SET (" + std::to_string(advanced_secret.length()) + " chars)") << std::endl;
        std::cout << "   Base URL: " << base_url << std::endl;
        
        setApiCredentials(advanced_api_key, advanced_secret, ""); // Advanced Trade API doesn't use passphrase
        setBaseUrl(base_url);
    }
    return curl_ != nullptr; 
}

void RestClient::cleanup() {
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
}

void RestClient::setApiCredentials(const std::string& api_key, const std::string& secret_key, const std::string& passphrase) {
    api_key_ = api_key;
    secret_key_ = secret_key;
    passphrase_ = passphrase;
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
    return get("/accounts", {}, true);
}

RestResponse RestClient::getOpenOrders(const std::string& symbol) { 
    std::map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["product_id"] = symbol;
    }
    return get("/orders/historical/batch", params, true);
}

RestResponse RestClient::getAllOrders(const std::string& symbol, const std::string& order_id) { 
    RestResponse response;
    response.success = true;
    return response;
}

RestResponse RestClient::placeOrder(const std::string& symbol, const std::string& side, 
                                   const std::string& type, const std::string& time_in_force,
                                   double quantity, double price, const std::string& client_order_id) {
    // Create Coinbase Advanced Trade API order payload
    nlohmann::json order_payload = {
        {"product_id", symbol},
        {"side", side == "BUY" ? "buy" : "sell"},
        {"order_configuration", {}}
    };
    
    // Set order configuration based on type
    if (type == "LIMIT") {
        order_payload["order_configuration"]["limit_limit_gtc"] = {
            {"base_size", std::to_string(quantity)},
            {"limit_price", std::to_string(price)}
        };
    } else if (type == "MARKET") {
        if (side == "BUY") {
            order_payload["order_configuration"]["market_market_ioc"] = {
                {"quote_size", std::to_string(quantity * price)}
            };
        } else {
            order_payload["order_configuration"]["market_market_ioc"] = {
                {"base_size", std::to_string(quantity)}
            };
        }
    }
    
    if (!client_order_id.empty()) {
        order_payload["client_order_id"] = client_order_id;
    }
    
    std::string body = order_payload.dump();
    
    // Use Coinbase Advanced Trade API endpoint
    auto response = postJson("/orders", body, true);
    
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

CoinbaseApiLimits RestClient::getApiLimits() const { return api_limits_; }
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
        
        // Setup headers for Coinbase
        struct curl_slist* headers = nullptr;
        if (!api_key_.empty() && requires_signature) {
            // Generate JWT token for Coinbase authentication
            std::string request_body = (method == "POST" && !query_string.empty()) ? query_string : "";
            std::string jwt_token = createJwtToken(method, endpoint, request_body);
            std::string auth_header = "Authorization: Bearer " + jwt_token;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        headers = curl_slist_append(headers, "Content-Type: application/json");
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

RestResponse RestClient::postJson(const std::string& endpoint, const std::string& json_body, bool requires_signature) {
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
        // Build full URL
        std::string url = base_url_ + endpoint;
        
        // Setup request
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response.response_body);
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json_body.c_str());
        
        // Setup headers for Coinbase
        struct curl_slist* headers = nullptr;
        if (!api_key_.empty() && requires_signature) {
            // Generate JWT token for Coinbase authentication
            std::string jwt_token = createJwtToken("POST", endpoint, json_body);
            std::string auth_header = "Authorization: Bearer " + jwt_token;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        
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

RestResponse RestClient::delete_request(const std::string& endpoint, const std::map<std::string, std::string>& params, bool requires_signature) {
    return makeRequest("DELETE", endpoint, params, requires_signature);
}

std::string RestClient::createJwtToken(const std::string& method, const std::string& request_path, const std::string& body) const {
    try {
        // Parse the private key and replace \\n with actual newlines
        std::string private_key = secret_key_;
        size_t pos = 0;
        while ((pos = private_key.find("\\n", pos)) != std::string::npos) {
            private_key.replace(pos, 2, "\n");
            pos += 1;
        }
        
        // Generate random nonce (16 bytes as raw binary, not hex)
        unsigned char nonce_raw[16];
        RAND_bytes(nonce_raw, sizeof(nonce_raw));
        std::string nonce(reinterpret_cast<char*>(nonce_raw), sizeof(nonce_raw));
        
        // Build URI for the request (method + space + host + path)
        std::string uri = method + " " + "api.coinbase.com" + request_path;
        
        // Create JWT token exactly like Coinbase example
        auto token = jwt::create()
            .set_subject(api_key_)
            .set_issuer("cdp")
            .set_not_before(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{120})
            .set_payload_claim("uri", jwt::claim(uri))
            .set_header_claim("kid", jwt::claim(api_key_))
            .set_header_claim("nonce", jwt::claim(nonce));
            
        // Add request body if present
        if (!body.empty()) {
            token.set_payload_claim("request_body", jwt::claim(body));
        }
        
        // Sign with ES256 using empty public key and private key
        // jwt-cpp will derive the public key from the private key
        return token.sign(jwt::algorithm::es256("", private_key));
        
    } catch (const std::exception& e) {
        std::cerr << "❌ JWT token creation failed: " << e.what() << std::endl;
        return "";
    }
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

// Manual base64UrlEncode removed - jwt-cpp handles this

// Manual ES256 signing removed - now using jwt-cpp library

std::string RestClient::base64Decode(const std::string& input) const {
    BIO *bio, *b64;
    int decode_len = input.length();
    std::vector<char> buffer(decode_len);
    
    bio = BIO_new_mem_buf(input.c_str(), -1);
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);
    
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    decode_len = BIO_read(bio, buffer.data(), input.length());
    BIO_free_all(bio);
    
    return std::string(buffer.data(), decode_len);
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

std::string RestClient::hexEncode(const std::string& input) const {
    std::ostringstream oss;
    for (char c : input) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
    }
    return oss.str();
} 