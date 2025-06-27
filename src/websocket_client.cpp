#include "websocket_client.h"
#include "config.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <libwebsockets.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <iomanip>
#include <chrono>
#include <vector>
#include <algorithm>
#include <jwt-cpp/jwt.h>

// Static pointer to access WebSocketClient instance from C callback
static WebSocketClient* client_instance = nullptr;

WebSocketClient::WebSocketClient() {
    last_message_time_ = std::chrono::system_clock::now();
    client_instance = this;
    
    // Initialize protocols
    protocols_[0] = {
        "coinbase-protocol",
        lwsCallback,
        0,
        4096,
        0, nullptr, 0
    };
    protocols_[1] = { nullptr, nullptr, 0, 0, 0, nullptr, 0 };
}

WebSocketClient::~WebSocketClient() { 
    // Clear static instance pointer to prevent callback access after destruction
    if (client_instance == this) {
        client_instance = nullptr;
    }
    
    // Ensure proper shutdown sequence
    if (running_ || connected_) {
        stop(); 
    }
    
    // Clean up context after all threads are stopped
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
}

void WebSocketClient::setApiCredentials(const std::string& api_key, const std::string& secret_key, const std::string& passphrase) {
    api_key_ = api_key;
    secret_key_ = secret_key;
    // Note: passphrase not needed for Advanced Trade API, but kept for compatibility
    passphrase_ = passphrase;
}

bool WebSocketClient::connect(const std::string& url) {
    if (running_) {
        std::cout << "WebSocket already running, stopping first..." << std::endl;
        stop();
    }
    
    url_ = url;
    
    // Parse URL to extract host, path, and port
    if (!parseUrl(url, host_, path_, port_)) {
        std::cout << "Failed to parse WebSocket URL: " << url << std::endl;
        return false;
    }
    
    std::cout << "Connecting to WebSocket: " << host_ << ":" << port_ << path_ << std::endl;
    
    // Start the worker thread
    worker_thread_ = std::make_unique<std::thread>([this]() {
        workerLoop();
    });
    
    // Wait a bit for connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    return true;
}

void WebSocketClient::disconnect() { 
    connected_ = false;
    std::cout << "WebSocket disconnecting..." << std::endl;
    
    if (wsi_) {
        lws_close_reason(wsi_, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        wsi_ = nullptr;
    }
}

bool WebSocketClient::isConnected() const { 
    return connected_; 
}

void WebSocketClient::setMessageCallback(MessageCallback callback) { 
    message_callback_ = callback; 
}

void WebSocketClient::setConnectionCallback(ConnectionCallback callback) { 
    connection_callback_ = callback; 
}

void WebSocketClient::setErrorCallback(ErrorCallback callback) { 
    error_callback_ = callback; 
}

bool WebSocketClient::subscribeOrderBook(const std::string& symbol, int depth, int update_speed_ms) {
    // Use Advanced Trade API with JWT/EdDSA authentication for Level 2 data
    if (api_key_.empty() || secret_key_.empty()) {
        std::cout << "❌ ERROR: Advanced Trade API credentials required for Level 2 data" << std::endl;
        return false;
    }
    
    // API credentials validated
    
    // Generate JWT token for Advanced Trade API WebSocket authentication
    std::string jwt_token = createJwtToken("", "", "");
    
    // Create subscription message with JWT authentication for Advanced Trade API
    nlohmann::json subscription = {
        {"type", "subscribe"},
        {"product_ids", {symbol}},
        {"channel", "level2"},
        {"jwt", jwt_token}
    };
    
    std::string sub_msg = subscription.dump();
    
    // JWT token generated
    
    if (connected_ && wsi_) {
        sendMessage(sub_msg);
    } else {
        pending_subscriptions_.push_back(sub_msg);
    }
    
    return true;
}

bool WebSocketClient::subscribeTicker(const std::string& symbol) { 
    return true; 
}

bool WebSocketClient::subscribeTrades(const std::string& symbol) { 
    return true; 
}

void WebSocketClient::unsubscribeAll() {
    pending_subscriptions_.clear();
}

void WebSocketClient::enablePing(int interval_seconds) { 
    ping_enabled_ = true; 
    ping_interval_seconds_ = interval_seconds;
}

void WebSocketClient::disablePing() { 
    ping_enabled_ = false; 
}

std::chrono::system_clock::time_point WebSocketClient::getLastMessageTime() const {
    std::lock_guard<std::mutex> lock(time_mutex_);
    return last_message_time_;
}

bool WebSocketClient::isHealthy() const { 
    return connected_; 
}

void WebSocketClient::printStats() const {
    std::cout << "WebSocket Stats - Messages: " << message_count_ 
              << " Errors: " << error_count_ << std::endl;
}

uint64_t WebSocketClient::getMessageCount() const { 
    return message_count_; 
}

uint64_t WebSocketClient::getErrorCount() const { 
    return error_count_; 
}

double WebSocketClient::getAverageLatency() const { 
    return total_latency_ > 0 ? total_latency_ / message_count_ : 0.0; 
}

void WebSocketClient::start() {
    running_ = true;
    std::cout << "WebSocket client started" << std::endl;
}

void WebSocketClient::stop() {
    std::cout << "Stopping WebSocket client..." << std::endl;
    
    // First, signal all threads to stop
    running_ = false;
    connected_ = false;
    ping_enabled_ = false;
    // Disable callbacks to prevent further processing
    message_callback_ = nullptr;
    connection_callback_ = nullptr;
    error_callback_ = nullptr;
    
    // Request libwebsockets service loop to exit quickly
    if (context_) {
        lws_cancel_service(context_);
    }

    // Close WebSocket connection if still open
    if (wsi_) {
        lws_close_reason(wsi_, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        wsi_ = nullptr;
    }

    // Join ping thread (with timeout)
    if (ping_thread_ && ping_thread_->joinable()) {
        if (ping_thread_->joinable()) {
            if (ping_thread_->joinable()) ping_thread_->join();
        }
        ping_thread_.reset();
    }

    // Join worker thread but don't wait forever (max 2 seconds)
    if (worker_thread_ && worker_thread_->joinable()) {
        auto join_start = std::chrono::steady_clock::now();
        while (worker_thread_->joinable()) {
            auto elapsed = std::chrono::steady_clock::now() - join_start;
            if (elapsed > std::chrono::seconds(2)) {
                std::cout << "Worker thread did not exit in time – detaching to avoid hang" << std::endl;
                worker_thread_->detach();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!worker_thread_->joinable()) {
            worker_thread_.reset();
        }
    }

    // Destroy context last
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    
    std::cout << "WebSocket client stopped" << std::endl;
}

void WebSocketClient::workerLoop() {
    running_ = true;
    
    try {
        // Initialize libwebsockets context creation info
        memset(&info_, 0, sizeof(info_));
        info_.port = CONTEXT_PORT_NO_LISTEN;
        info_.protocols = protocols_;
        info_.gid = -1;
        info_.uid = -1;
        info_.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info_.ka_time = 30;
        info_.ka_probes = 5;
        info_.ka_interval = 5;
        
        // Create the context
        context_ = lws_create_context(&info_);
        if (!context_) {
            std::cout << "❌ Failed to create libwebsockets context" << std::endl;
            handleError("Failed to create libwebsockets context");
            return;
        }
        
        // libwebsockets context created
        
        // Prepare connection info
        struct lws_client_connect_info ccinfo = {0};
        ccinfo.context = context_;
        ccinfo.address = host_.c_str();
        ccinfo.port = port_;
        ccinfo.path = path_.c_str();
        ccinfo.host = ccinfo.address;
        ccinfo.origin = ccinfo.address;
        ccinfo.protocol = protocols_[0].name;
        ccinfo.ssl_connection = (port_ == 443) ? LCCSCF_USE_SSL : 0;
        
        // Connecting to WebSocket
        
        // Connect
        wsi_ = lws_client_connect_via_info(&ccinfo);
        if (!wsi_) {
            std::cout << "❌ Failed to create WebSocket connection" << std::endl;
            handleError("Failed to create WebSocket connection");
            return;
        }
        
        // WebSocket connection initiated
        
        // Main service loop
        while (running_.load()) {
            // Service libwebsockets with short timeout
            int result = lws_service(context_, 50);
            
            // Check if service failed or context was destroyed
            if (result < 0) {
                std::cout << "lws_service error, terminating worker loop" << std::endl;
                break;
            }
        
            // Send any queued messages
            flushTxQueue();
            
            // Small delay to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
    } catch (const std::exception& e) {
        std::cout << "WebSocket worker exception: " << e.what() << std::endl;
        handleError(e.what());
    }
    
    running_ = false;
    connected_ = false;
}

void WebSocketClient::pingLoop() {
    const std::chrono::milliseconds step(200);
    std::chrono::milliseconds elapsed{0};
    while (running_ && ping_enabled_) {
        if (elapsed >= std::chrono::seconds(ping_interval_seconds_)) {
            if (connected_ && wsi_) {
                unsigned char ping_payload[LWS_PRE + 10];
                lws_write(wsi_, &ping_payload[LWS_PRE], 0, LWS_WRITE_PING);
            }
            elapsed = std::chrono::milliseconds{0};
        }
        std::this_thread::sleep_for(step);
        elapsed += step;
    }
}

void WebSocketClient::handleConnect() {
    std::cout << "✅ WebSocket connection established!" << std::endl;
    connected_ = true;
    
    {
        std::lock_guard<std::mutex> lock(time_mutex_);
        last_message_time_ = std::chrono::system_clock::now();
    }
    
    if (connection_callback_) {
        connection_callback_(true);
    }
    
    // Send pending subscriptions
    for (const auto& sub : pending_subscriptions_) {
        sendMessage(sub);
        std::cout << "📡 Sent pending subscription: " << sub << std::endl;
    }
    pending_subscriptions_.clear();
    
    // Start ping thread if enabled
    if (ping_enabled_) {
        ping_thread_ = std::make_unique<std::thread>([this]() {
            pingLoop();
        });
    }
}

void WebSocketClient::handleDisconnect() {
    std::cout << "❌ WebSocket connection closed" << std::endl;
    connected_ = false;
    
    if (connection_callback_) {
        connection_callback_(false);
    }
}

void WebSocketClient::handleMessage(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(time_mutex_);
        last_message_time_ = std::chrono::system_clock::now();
    }
    
    message_count_++;
    
    try {
        nlohmann::json json_msg = nlohmann::json::parse(message);
        
        // Enhanced error logging - show full error details
        if (message.find("error") != std::string::npos) {
            std::cout << "❌ ERROR MESSAGE RECEIVED:" << std::endl;
            std::cout << "   Raw: " << message << std::endl;
            
            // Try to extract additional error details
            if (json_msg.contains("message")) {
                std::cout << "   Error message: " << json_msg["message"] << std::endl;
            }
            if (json_msg.contains("error")) {
                std::cout << "   Error field: " << json_msg["error"] << std::endl;
            }
            if (json_msg.contains("code")) {
                std::cout << "   Error code: " << json_msg["code"] << std::endl;
            }
            if (json_msg.contains("details")) {
                std::cout << "   Error details: " << json_msg["details"] << std::endl;
            }
        }
        
        // Message processing (silent)
        
        if (message_callback_) {
            message_callback_(json_msg);
        }
        
        processMessage(json_msg);
        
    } catch (const std::exception& e) {
        std::cout << "Error parsing JSON message: " << e.what() << std::endl;
        std::cout << "Raw message: " << message.substr(0, 500) << std::endl;
        error_count_++;
    }
}

void WebSocketClient::handleError(const std::string& error) {
    error_count_++;
    std::cout << "WebSocket error: " << error << std::endl;
    
    if (error_callback_) {
        error_callback_(error);
    }
}

bool WebSocketClient::attemptReconnect() {
    // Implementation for reconnection logic
    return false;
}

void WebSocketClient::processMessage(const nlohmann::json& json_msg) {
    // This function intentionally does nothing as message processing
    // is handled by the message callback set in main.cpp
    // The callback updates the order book and generates trading signals
}

bool WebSocketClient::validateMessage(const nlohmann::json& json_msg) {
    return !json_msg.empty();
}

void WebSocketClient::updateLatencyStats(const nlohmann::json& json_msg) {
    // Update latency statistics if needed
}

std::string WebSocketClient::buildOrderBookSubscription(const std::string& symbol, int depth, int update_speed_ms) {
    std::string lowercase_symbol = symbol;
    std::transform(lowercase_symbol.begin(), lowercase_symbol.end(), lowercase_symbol.begin(), ::tolower);
    return lowercase_symbol + "@depth" + std::to_string(depth) + "@" + std::to_string(update_speed_ms) + "ms";
            }

std::string WebSocketClient::buildTickerSubscription(const std::string& symbol) {
    std::string lowercase_symbol = symbol;
    std::transform(lowercase_symbol.begin(), lowercase_symbol.end(), lowercase_symbol.begin(), ::tolower);
    return lowercase_symbol + "@ticker";
        }
        
std::string WebSocketClient::buildTradeSubscription(const std::string& symbol) {
    std::string lowercase_symbol = symbol;
    std::transform(lowercase_symbol.begin(), lowercase_symbol.end(), lowercase_symbol.begin(), ::tolower);
    return lowercase_symbol + "@trade";
    }

void WebSocketClient::checkHealth() {
    // Health check implementation
}

bool WebSocketClient::isMessageTimeoutExceeded() const {
    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_message_time_);
    return duration.count() > message_timeout_seconds_;
}

bool WebSocketClient::parseUrl(const std::string& url, std::string& host, std::string& path, int& port) {
    // Parse WebSocket URL (ws:// or wss://)
    std::string protocol;
    std::string remaining = url;
    
    if (remaining.find("wss://") == 0) {
        protocol = "wss";
        remaining = remaining.substr(6);
        port = 443;
    } else if (remaining.find("ws://") == 0) {
        protocol = "ws";
        remaining = remaining.substr(5);
        port = 80;
    } else {
        return false;
    }
    
    // Find path separator
    size_t path_pos = remaining.find('/');
    if (path_pos != std::string::npos) {
        host = remaining.substr(0, path_pos);
        path = remaining.substr(path_pos);
    } else {
        host = remaining;
        path = "/";
    }
    
    // Check for port in host
    size_t port_pos = host.find(':');
    if (port_pos != std::string::npos) {
        port = std::stoi(host.substr(port_pos + 1));
        host = host.substr(0, port_pos);
    }
    
    return true;
}

int WebSocketClient::lwsCallback(struct lws *wsi, enum lws_callback_reasons reason,
                                void *user, void *in, size_t len) {
    
    if (!client_instance || !client_instance->running_) return 0;
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            client_instance->handleConnect();
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (in && len > 0) {
                // Handle message fragmentation for libwebsockets
                std::string fragment(static_cast<char*>(in), len);
                client_instance->rx_buffer_ += fragment;
    
                // Check if this is the final frame
                if (lws_is_final_fragment(wsi)) {
                    client_instance->handleMessage(client_instance->rx_buffer_);
                    client_instance->rx_buffer_.clear();
                }
            }
            break;
            
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            client_instance->handleError("Connection error");
            break;
            
        case LWS_CALLBACK_CLOSED:
            client_instance->wsi_ = nullptr;  // Important: clear the WSI pointer
            client_instance->handleDisconnect();
            break;
            
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            client_instance->flushTxQueue();
            break;
            
        default:
            break;
    }
    
    return 0;
}

void WebSocketClient::sendMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    tx_queue_.push_back(message);
    
    // Outgoing message queued
    
    if (wsi_) {
        lws_callback_on_writable(wsi_);
    }
}

void WebSocketClient::flushTxQueue() {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    
    while (!tx_queue_.empty() && wsi_) {
        const std::string& message = tx_queue_.front();
        
        // Prepare buffer with LWS_PRE padding
        size_t msg_len = message.length();
        std::vector<unsigned char> buffer(LWS_PRE + msg_len);
        memcpy(&buffer[LWS_PRE], message.c_str(), msg_len);
        
        // Send the message
        int n = lws_write(wsi_, &buffer[LWS_PRE], msg_len, LWS_WRITE_TEXT);
        if (n < 0) {
            std::cout << "❌ Failed to send WebSocket message" << std::endl;
            handleError("Failed to send message");
            break;
        }
        
        tx_queue_.erase(tx_queue_.begin());
    }
} 

// JWT Authentication Implementation for Coinbase using jwt-cpp
std::string WebSocketClient::createJwtToken(const std::string& method, const std::string& request_path, const std::string& body) const {
    try {
        // Parse the private key and replace \\n with actual newlines
        std::string private_key = secret_key_;
        size_t pos = 0;
        while ((pos = private_key.find("\\n", pos)) != std::string::npos) {
            private_key.replace(pos, 2, "\n");
            pos += 1;
        }
        
        // Generate random nonce (16 bytes as raw binary)
        unsigned char nonce_raw[16];
        RAND_bytes(nonce_raw, sizeof(nonce_raw));
        std::string nonce(reinterpret_cast<char*>(nonce_raw), sizeof(nonce_raw));
        
        // For WebSocket, use the same host as REST API for JWT validation
        std::string uri = "GET api.coinbase.com";
        
        // Create JWT token exactly like Coinbase example
        auto token = jwt::create()
            .set_subject(api_key_)
            .set_issuer("cdp")
            .set_not_before(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{120})
            .set_payload_claim("uri", jwt::claim(uri))
            .set_header_claim("kid", jwt::claim(api_key_))
            .set_header_claim("nonce", jwt::claim(nonce))
            .sign(jwt::algorithm::es256("", private_key));
        
        return token;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ WebSocket JWT token creation failed: " << e.what() << std::endl;
        return "";
    }
}

// Manual base64UrlEncode removed - jwt-cpp handles this

// Manual signing methods removed - now using jwt-cpp library

std::string WebSocketClient::createHMACSignature(const std::string& message, const std::string& secret) const {
    std::cout << "🔐 Creating HMAC-SHA256 signature:" << std::endl;
    std::cout << "   Message: " << message << std::endl;
    std::cout << "   Secret length: " << secret.length() << " chars" << std::endl;
    
    // For Exchange API, decode the base64 secret key
    std::string decoded_secret;
    std::cout << "   Processing Exchange API secret key for HMAC..." << std::endl;
    std::cout << "   Secret key length: " << secret.length() << " chars" << std::endl;
    std::cout << "   Secret key sample: " << secret.substr(0, 50) << "..." << std::endl;
    
    // Try to base64 decode the secret
    decoded_secret = base64Decode(secret);
    if (decoded_secret.length() > 0) {
        std::cout << "   Successfully decoded to " << decoded_secret.length() << " bytes for HMAC" << std::endl;
    } else {
        // Fallback: use the secret as-is
        std::cout << "   Decode failed, using secret as-is..." << std::endl;
        decoded_secret = secret;
    }
    
    // Create HMAC-SHA256 signature
    unsigned char* digest;
    unsigned int digest_len;
    
    digest = HMAC(EVP_sha256(), 
                  decoded_secret.c_str(), decoded_secret.length(),
                  reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
                  nullptr, &digest_len);
    
    if (!digest) {
        std::cout << "❌ Failed to create HMAC signature" << std::endl;
        return "";
    }
    
    // Convert to base64
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;
    
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, digest, digest_len);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    
    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);
    
    std::cout << "✅ HMAC signature created (length: " << result.length() << " chars)" << std::endl;
    return result;
}

std::string WebSocketClient::base64Decode(const std::string& input) const {
    BIO *bio, *b64;
    std::vector<char> buffer(input.length());
    
    bio = BIO_new_mem_buf(input.c_str(), input.length());
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);
    
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    int decode_len = BIO_read(bio, buffer.data(), buffer.size());
    BIO_free_all(bio);
    
    if (decode_len <= 0) {
        std::cout << "   Base64 decode failed, decode_len = " << decode_len << std::endl;
        std::cout << "   Input length: " << input.length() << std::endl;
        std::cout << "   Input sample: " << input.substr(0, 50) << "..." << std::endl;
        return "";
    }
    
    std::cout << "   Base64 decode successful, decoded " << decode_len << " bytes" << std::endl;
    return std::string(buffer.data(), decode_len);
}

std::string WebSocketClient::hexEncode(const std::string& input) const {
    std::stringstream ss;
    for (char c : input) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)(unsigned char)c;
    }
    return ss.str();
}