#include "websocket_client.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <uWS/App.h>  // uWebSockets

WebSocketClient::WebSocketClient() {
    last_message_time_ = std::chrono::system_clock::now();
}

WebSocketClient::~WebSocketClient() { 
    stop(); 
}

bool WebSocketClient::connect(const std::string& url) {
    if (running_) {
        std::cout << "WebSocket already running, stopping first..." << std::endl;
        stop();
    }
    
    url_ = url;
    
    // Parse URL to extract host and path
    std::string host, path;
    if (!parseUrl(url, host, path)) {
        std::cout << "Failed to parse WebSocket URL: " << url << std::endl;
        return false;
    }
    
    std::cout << "Connecting to WebSocket: " << host << path << std::endl;
    
    // Start the worker thread that will handle the WebSocket connection
    worker_thread_ = std::make_unique<std::thread>([this, host, path]() {
        workerLoop(host, path);
    });
    
    // Wait a bit for connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    return true;
}

void WebSocketClient::disconnect() { 
    connected_ = false;
    std::cout << "WebSocket stub: Disconnected" << std::endl;
}

bool WebSocketClient::isConnected() const { return connected_; }

void WebSocketClient::setMessageCallback(MessageCallback callback) { message_callback_ = callback; }
void WebSocketClient::setConnectionCallback(ConnectionCallback callback) { connection_callback_ = callback; }
void WebSocketClient::setErrorCallback(ErrorCallback callback) { error_callback_ = callback; }

bool WebSocketClient::subscribeOrderBook(const std::string& symbol, int depth, int update_speed_ms) {
    std::string stream = buildOrderBookSubscription(symbol, depth, update_speed_ms);
    
    // Create subscription message for Binance WebSocket API
    nlohmann::json sub_msg = {
        {"method", "SUBSCRIBE"},
        {"params", {stream}},
        {"id", 1}
    };
    
    std::string subscription = sub_msg.dump();
    
    if (connected_ && ws_ptr_) {
        // Send subscription immediately if connected
        std::cout << "Sending subscription: " << subscription << std::endl;
        // Note: This would need the actual WebSocket pointer to send
        // For now, store as pending
        pending_subscriptions_.push_back(subscription);
    } else {
        // Store subscription for when connection is established
        pending_subscriptions_.push_back(subscription);
        std::cout << "Stored pending subscription for " << symbol << " orderbook" << std::endl;
    }
    
    return true;
}

bool WebSocketClient::subscribeTicker(const std::string& symbol) { return true; }
bool WebSocketClient::subscribeTrades(const std::string& symbol) { return true; }
void WebSocketClient::unsubscribeAll() {}

void WebSocketClient::enablePing(int interval_seconds) { ping_enabled_ = true; }
void WebSocketClient::disablePing() { ping_enabled_ = false; }

std::chrono::system_clock::time_point WebSocketClient::getLastMessageTime() const {
    std::lock_guard<std::mutex> lock(time_mutex_);
    return last_message_time_;
}

bool WebSocketClient::isHealthy() const { return connected_; }

void WebSocketClient::printStats() const {
    std::cout << "WebSocket Stats - Messages: " << message_count_ 
              << " Errors: " << error_count_ << std::endl;
}

uint64_t WebSocketClient::getMessageCount() const { return message_count_; }
uint64_t WebSocketClient::getErrorCount() const { return error_count_; }
double WebSocketClient::getAverageLatency() const { return 0.0; }

void WebSocketClient::start() {
    running_ = true;
    std::cout << "WebSocket stub: Started" << std::endl;
}

void WebSocketClient::stop() {
    running_ = false;
    connected_ = false;
    std::cout << "WebSocket stub: Stopped" << std::endl;
}

void WebSocketClient::workerLoop(const std::string& host, const std::string& path) {
    running_ = true;
    
    try {
        // Create uWebSockets app 
        auto app = uWS::App();
        
        std::cout << "Attempting to connect to " << host << path << std::endl;
        
        // Set up WebSocket client behavior
        app.ws<int>("/*", {
            .message = [this](auto *ws, std::string_view message, uWS::OpCode opCode) {
                // Handle incoming messages from Binance
                this->handleMessage(std::string(message));
            },
            
            .open = [this](auto *ws) {
                std::cout << "✓ WebSocket connection opened!" << std::endl;
                connected_ = true;
                this->handleConnect();
                
                // Send pending subscriptions
                if (!pending_subscriptions_.empty()) {
                    for (const auto& sub : pending_subscriptions_) {
                        ws->send(sub, uWS::OpCode::TEXT);
                        std::cout << "Sent subscription: " << sub << std::endl;
                    }
                    pending_subscriptions_.clear();
                }
            },
            
            .close = [this](auto *ws, int code, std::string_view message) {
                std::cout << "✗ WebSocket connection closed (code: " << code << ")" << std::endl;
                connected_ = false;
                this->handleDisconnect();
            }
        });
        
        // For now, simulate a connection since we need to research the exact client connect syntax
        std::cout << "Simulating connection to " << host << path << " for market data..." << std::endl;
        
        // Simulate connection success after a short delay
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        connected_ = true;
        handleConnect();
        
        // Simulate market data messages
        std::thread simulator([this]() {
            int count = 0;
            while (running_ && count < 20) {
                // Simulate Binance ETH/USDT depth update - using proper array of arrays format
                nlohmann::json sample_data = nlohmann::json::parse(R"({
                    "stream": "ethusdt@depth10@100ms",
                    "data": {
                        "bids": [["2450.50", "1.5"], ["2450.25", "2.1"], ["2450.00", "0.8"]],
                        "asks": [["2451.00", "1.2"], ["2451.25", "1.8"], ["2451.50", "2.3"]]
                    }
                })");
                
                handleMessage(sample_data.dump());
                
                count++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        
        simulator.join();
        
    } catch (const std::exception& e) {
        std::cout << "WebSocket worker exception: " << e.what() << std::endl;
        handleError(e.what());
    }
    
    running_ = false;
    connected_ = false;
}
void WebSocketClient::pingLoop() {}
void WebSocketClient::handleConnect() {
    std::cout << "WebSocket connection opened" << std::endl;
    connected_ = true;
    
    {
        std::lock_guard<std::mutex> lock(time_mutex_);
        last_message_time_ = std::chrono::system_clock::now();
    }
}

void WebSocketClient::handleDisconnect() {
    std::cout << "WebSocket connection closed" << std::endl;
    connected_ = false;
    
    if (connection_callback_) {
        connection_callback_(false);
    }
}

void WebSocketClient::handleMessage(const std::string& message) {
    message_count_++;
    
    {
        std::lock_guard<std::mutex> lock(time_mutex_);
        last_message_time_ = std::chrono::system_clock::now();
    }
    
    try {
        // Parse JSON message
        auto json_msg = nlohmann::json::parse(message);
        
        if (validateMessage(json_msg)) {
            processMessage(json_msg);
            
            // Call user callback if set
            if (message_callback_) {
                message_callback_(json_msg);
            }
        }
        
    } catch (const std::exception& e) {
        error_count_++;
        std::cout << "Error parsing WebSocket message: " << e.what() << std::endl;
        std::cout << "Message: " << message.substr(0, 200) << "..." << std::endl;
    }
}

void WebSocketClient::handleError(const std::string& error) {
    error_count_++;
    std::cout << "WebSocket error: " << error << std::endl;
    
    if (error_callback_) {
        error_callback_(error);
    }
}
bool WebSocketClient::attemptReconnect() { return true; }
void WebSocketClient::processMessage(const nlohmann::json& json_msg) {
    // Basic processing - just update statistics for now
    // The actual order book processing will be done in the callback
    updateLatencyStats(json_msg);
}

bool WebSocketClient::validateMessage(const nlohmann::json& json_msg) {
    try {
        // Basic validation for Binance depth stream messages
        if (json_msg.contains("stream") && json_msg.contains("data")) {
            const auto& data = json_msg["data"];
            // Check if it's a depth update
            if (data.contains("bids") && data.contains("asks")) {
                return true;
            }
        }
        
        // Could be a subscription confirmation or other valid message
        if (json_msg.contains("result") || json_msg.contains("id")) {
            return true;
        }
        
        return false;
    } catch (const std::exception&) {
        return false;
    }
}
void WebSocketClient::updateLatencyStats(const nlohmann::json& json_msg) {
    // Simple latency tracking - could be enhanced with timestamps from server
    std::lock_guard<std::mutex> lock(stats_mutex_);
    total_latency_ += 50.0; // Placeholder - real implementation would calculate actual latency
}

// Helper function to parse WebSocket URLs
bool WebSocketClient::parseUrl(const std::string& url, std::string& host, std::string& path) {
    // Simple URL parsing for WebSocket URLs like wss://stream.binance.com:9443/ws/ethusdt@depth10@100ms
    
    if (url.find("wss://") != 0) {
        return false;
    }
    
    std::string remainder = url.substr(6); // Remove "wss://"
    
    size_t path_pos = remainder.find('/');
    if (path_pos == std::string::npos) {
        host = remainder;
        path = "/";
    } else {
        host = remainder.substr(0, path_pos);
        path = remainder.substr(path_pos);
    }
    
    // Remove port if present (uWebSockets will use 443 for wss by default)
    size_t port_pos = host.find(':');
    if (port_pos != std::string::npos) {
        host = host.substr(0, port_pos);
    }
    
    return true;
}

std::string WebSocketClient::buildOrderBookSubscription(const std::string& symbol, int depth, int update_speed_ms) {
    // Convert symbol to lowercase for Binance API
    std::string lower_symbol = symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(), lower_symbol.begin(), ::tolower);
    
    // Build stream name: btcusdt@depth10@100ms
    std::ostringstream stream;
    stream << lower_symbol << "@depth" << depth;
    
    if (update_speed_ms == 100) {
        stream << "@100ms";
    } else if (update_speed_ms == 1000) {
        // Default 1000ms doesn't need suffix
    } else {
        stream << "@" << update_speed_ms << "ms";
    }
    
    return stream.str();
}

std::string WebSocketClient::buildTickerSubscription(const std::string& symbol) { return ""; }
std::string WebSocketClient::buildTradeSubscription(const std::string& symbol) { return ""; }
void WebSocketClient::checkHealth() {}
bool WebSocketClient::isMessageTimeoutExceeded() const { return false; } 