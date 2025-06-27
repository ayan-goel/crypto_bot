#pragma once

#include "order_book.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>
#include <nlohmann/json.hpp>
#include <libwebsockets.h>

class WebSocketClient {
public:
    // Callback types
    using MessageCallback = std::function<void(const nlohmann::json&)>;
    using ConnectionCallback = std::function<void(bool connected)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    WebSocketClient();
    ~WebSocketClient();
    
    // Authentication setup for Coinbase Advanced Trade API
    void setApiCredentials(const std::string& api_key, const std::string& secret_key, const std::string& passphrase = "");
    
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
    // Authentication credentials
    std::string api_key_;
    std::string secret_key_;
    std::string passphrase_;
    
    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::string url_;
    std::string host_;
    std::string path_;
    int port_{443};
    
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
    
    // libwebsockets members
    struct lws_context *context_ = nullptr;
    struct lws *wsi_ = nullptr;
    struct lws_protocols protocols_[2];
    struct lws_context_creation_info info_;
    
    // Message buffer for libwebsockets
    std::string rx_buffer_;
    std::vector<std::string> tx_queue_;
    std::mutex tx_mutex_;
    
    // Worker methods
    void workerLoop();
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
    bool parseUrl(const std::string& url, std::string& host, std::string& path, int& port);
    
    // libwebsockets callback
    static int lwsCallback(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len);
    
    // Send message via libwebsockets
    void sendMessage(const std::string& message);
    void flushTxQueue();
    
    // JWT authentication for Coinbase
    std::string createJwtToken(const std::string& method, const std::string& request_path, const std::string& body = "") const;
    std::string createHMACSignature(const std::string& message, const std::string& secret) const;
    std::string base64Decode(const std::string& input) const;
    std::string hexEncode(const std::string& input) const;
}; 