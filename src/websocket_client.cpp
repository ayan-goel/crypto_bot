#include "websocket_client.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <libwebsockets.h>

// Static pointer to access WebSocketClient instance from C callback
static WebSocketClient* client_instance = nullptr;

WebSocketClient::WebSocketClient() {
    last_message_time_ = std::chrono::system_clock::now();
    client_instance = this;
    
    // Initialize protocols
    protocols_[0] = {
        "binance-protocol",
        lwsCallback,
        0,
        4096,
        0, nullptr, 0
    };
    protocols_[1] = { nullptr, nullptr, 0, 0, 0, nullptr, 0 };
}

WebSocketClient::~WebSocketClient() { 
    stop(); 
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
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
    std::string stream = buildOrderBookSubscription(symbol, depth, update_speed_ms);
    
    // Create subscription message for Binance WebSocket API
    nlohmann::json sub_msg = {
        {"method", "SUBSCRIBE"},
        {"params", {stream}},
        {"id", 1}
    };
    
    std::string subscription = sub_msg.dump();
    
    if (connected_ && wsi_) {
        sendMessage(subscription);
        std::cout << "Sent subscription: " << subscription << std::endl;
    } else {
        // Store subscription for when connection is established
        pending_subscriptions_.push_back(subscription);
        std::cout << "Stored pending subscription for " << symbol << " orderbook" << std::endl;
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
    running_ = false;
    connected_ = false;
    
    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }
    
    if (ping_thread_ && ping_thread_->joinable()) {
        ping_thread_->join();
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
        
        std::cout << "✅ Created libwebsockets context" << std::endl;
        
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
        
        std::cout << "🔗 Connecting to " << host_ << ":" << port_ << path_ << std::endl;
        
        // Connect
        wsi_ = lws_client_connect_via_info(&ccinfo);
        if (!wsi_) {
            std::cout << "❌ Failed to create WebSocket connection" << std::endl;
            handleError("Failed to create WebSocket connection");
            return;
        }
        
        std::cout << "🚀 WebSocket connection initiated..." << std::endl;
        
        // Main service loop
        while (running_) {
            lws_service(context_, 50);
            
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
    while (running_ && ping_enabled_) {
        std::this_thread::sleep_for(std::chrono::seconds(ping_interval_seconds_));
        
        if (connected_ && wsi_) {
            // Send ping frame using libwebsockets
            unsigned char ping_payload[LWS_PRE + 10];
            lws_write(wsi_, &ping_payload[LWS_PRE], 0, LWS_WRITE_PING);
        }
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
        
        if (message_callback_) {
            message_callback_(json_msg);
        }
        
        processMessage(json_msg);
        
    } catch (const std::exception& e) {
        std::cout << "Error parsing JSON message: " << e.what() << std::endl;
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
    // Process the JSON message (implement your business logic here)
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
    
    if (!client_instance) return 0;
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            client_instance->handleConnect();
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (in && len > 0) {
                std::string message(static_cast<char*>(in), len);
                client_instance->handleMessage(message);
            }
            break;
            
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            client_instance->handleError("Connection error");
            break;
            
        case LWS_CALLBACK_CLOSED:
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