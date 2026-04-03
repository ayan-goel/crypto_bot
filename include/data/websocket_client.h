#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>
#include <libwebsockets.h>

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const nlohmann::json&)>;

    WebSocketClient();
    ~WebSocketClient();

    void setApiCredentials(const std::string& api_key, const std::string& secret_key);

    bool connect(const std::string& url);
    void disconnect();

    void setMessageCallback(MessageCallback callback);
    bool subscribeOrderBook(const std::string& symbol, int depth = 10, int update_speed_ms = 100);

private:
    std::string api_key_;
    std::string secret_key_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::string url_;
    std::string host_;
    std::string path_;
    int port_{443};

    std::vector<std::string> pending_subscriptions_;

    std::thread worker_thread_;

    MessageCallback message_callback_;

    std::atomic<uint64_t> message_count_{0};
    std::atomic<uint64_t> error_count_{0};

    struct lws_context* context_ = nullptr;
    struct lws* wsi_ = nullptr;
    struct lws_protocols protocols_[2];
    struct lws_context_creation_info info_;

    std::string rx_buffer_;
    std::vector<std::string> tx_queue_;
    std::mutex tx_mutex_;

    void workerLoop();
    void stop();

    void handleConnect();
    void handleDisconnect();
    void handleMessage(const std::string& message);
    void handleError(const std::string& error);

    bool parseUrl(const std::string& url, std::string& host, std::string& path, int& port);

    static int lwsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                           void* user, void* in, size_t len);

    void sendMessage(const std::string& message);
    void flushTxQueue();

    std::string createJwtToken() const;
};
