#pragma once

#include "order_book.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>
#include <nlohmann/json.hpp>

// Forward declaration to avoid including uWS headers in header file
namespace uWS {
    template <bool SSL, bool isServer, typename USERDATA>
    struct WebSocket;
    
    template <bool SSL>
    struct TemplatedApp;
}

class WebSocketClient {
public:
    // Callback types
    using MessageCallback = std::function<void(const nlohmann::json&)>;
    using ConnectionCallback = std::function<void(bool connected)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    WebSocketClient();
    ~WebSocketClient();
    
    // Connection management
    bool connect(const std::string& url);
    void disconnect();
    bool isConnected() const;
    
    // Callbacks
    void setMessageCallback(MessageCallback callback);
    void setConnectionCallback(ConnectionCallback callback);
    void setErrorCallback(ErrorCallback callback);
    
    // Subscription management
    bool subscribeOrderBook(const std::string& symbol, int depth = 10, int update_speed_ms = 100);
    bool subscribeTicker(const std::string& symbol);
    bool subscribeTrades(const std::string& symbol);
    void unsubscribeAll();
    
    // Health monitoring
    void enablePing(int interval_seconds = 30);
    void disablePing();
    std::chrono::system_clock::time_point getLastMessageTime() const;
    bool isHealthy() const;
    
    // Statistics
    void printStats() const;
    uint64_t getMessageCount() const;
    uint64_t getErrorCount() const;
    double getAverageLatency() const;
    
    // Control
    void start();
    void stop();

private:
    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::string url_;
    
    // Pending subscriptions (sent when connection is established)
    std::vector<std::string> pending_subscriptions_;
    
    // Thread management
    std::unique_ptr<std::thread> worker_thread_;
    std::unique_ptr<std::thread> ping_thread_;
    
    // Callbacks
    MessageCallback message_callback_;
    ConnectionCallback connection_callback_;
    ErrorCallback error_callback_;
    
    // Health monitoring
    std::atomic<bool> ping_enabled_{false};
    int ping_interval_seconds_ = 30;
    std::chrono::system_clock::time_point last_message_time_;
    std::chrono::system_clock::time_point last_ping_time_;
    mutable std::mutex time_mutex_;
    
    // Statistics
    std::atomic<uint64_t> message_count_{0};
    std::atomic<uint64_t> error_count_{0};
    std::atomic<uint64_t> reconnect_count_{0};
    double total_latency_ = 0.0;
    std::mutex stats_mutex_;
    
    // Configuration
    int max_reconnect_attempts_ = 5;
    int reconnect_delay_seconds_ = 5;
    int message_timeout_seconds_ = 60;
    
    // uWebSockets members (using pointers to avoid header dependencies)
    void* app_ptr_ = nullptr;  // Will be cast to uWS::App*
    void* ws_ptr_ = nullptr;   // Will be cast to uWS::WebSocket*
    
    // Worker methods
    void workerLoop(const std::string& host, const std::string& path);
    void pingLoop();
    
    // Connection handling
    void handleConnect();
    void handleDisconnect();
    void handleMessage(const std::string& message);
    void handleError(const std::string& error);
    
    // Reconnection
    bool attemptReconnect();
    
    // Message processing
    void processMessage(const nlohmann::json& json_msg);
    bool validateMessage(const nlohmann::json& json_msg);
    void updateLatencyStats(const nlohmann::json& json_msg);
    
    // Subscription helpers
    std::string buildOrderBookSubscription(const std::string& symbol, int depth, int update_speed_ms);
    std::string buildTickerSubscription(const std::string& symbol);
    std::string buildTradeSubscription(const std::string& symbol);
    
    // Health checks
    void checkHealth();
    bool isMessageTimeoutExceeded() const;
    
    // URL parsing
    bool parseUrl(const std::string& url, std::string& host, std::string& path);
    
    // Real Binance data connection
    void connectToRealBinanceStream(const std::string& host);
    std::string fetchRealOrderBookData(const std::string& host);
    static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* response);
    std::string convertRestToStreamFormat(const std::string& rest_response);
}; 