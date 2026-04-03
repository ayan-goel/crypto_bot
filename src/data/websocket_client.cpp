#include "data/websocket_client.h"
#include <iostream>
#include <cstring>
#include <openssl/rand.h>
#include <jwt-cpp/jwt.h>

static WebSocketClient* client_instance = nullptr;

WebSocketClient::WebSocketClient() {
    client_instance = this;

    protocols_[0] = {
        "coinbase-protocol",
        lwsCallback,
        0,
        4096,
        0, nullptr, 0
    };
    protocols_[1] = {nullptr, nullptr, 0, 0, 0, nullptr, 0};
}

WebSocketClient::~WebSocketClient() {
    if (client_instance == this) {
        client_instance = nullptr;
    }

    if (running_ || connected_) {
        stop();
    }

    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
}

void WebSocketClient::setApiCredentials(const std::string& api_key, const std::string& secret_key) {
    api_key_ = api_key;
    secret_key_ = secret_key;
}

bool WebSocketClient::connect(const std::string& url) {
    if (running_) {
        std::cout << "WebSocket already running, stopping first..." << std::endl;
        stop();
    }

    url_ = url;

    if (!parseUrl(url, host_, path_, port_)) {
        std::cout << "Failed to parse WebSocket URL: " << url << std::endl;
        return false;
    }

    std::cout << "Connecting to WebSocket: " << host_ << ":" << port_ << path_ << std::endl;

    worker_thread_ = std::thread([this]() { workerLoop(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    return running_.load();
}

void WebSocketClient::disconnect() {
    connected_ = false;
    std::cout << "WebSocket disconnecting..." << std::endl;

    if (wsi_) {
        lws_close_reason(wsi_, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        wsi_ = nullptr;
    }
}

void WebSocketClient::setMessageCallback(MessageCallback callback) {
    message_callback_ = callback;
}

bool WebSocketClient::subscribeOrderBook(const std::string& symbol, int /*depth*/, int /*update_speed_ms*/) {
    if (api_key_.empty() || secret_key_.empty()) {
        std::cout << "ERROR: Advanced Trade API credentials required for Level 2 data" << std::endl;
        return false;
    }

    std::string jwt_token = createJwtToken();

    nlohmann::json subscription = {
        {"type", "subscribe"},
        {"product_ids", {symbol}},
        {"channel", "level2"},
        {"jwt", jwt_token}
    };

    std::string sub_msg = subscription.dump();

    if (connected_ && wsi_) {
        sendMessage(sub_msg);
    } else {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        pending_subscriptions_.push_back(sub_msg);
    }

    return true;
}

void WebSocketClient::stop() {
    std::cout << "Stopping WebSocket client..." << std::endl;

    running_ = false;
    connected_ = false;
    message_callback_ = nullptr;

    if (context_) {
        lws_cancel_service(context_);
    }

    if (wsi_) {
        lws_close_reason(wsi_, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        wsi_ = nullptr;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }

    std::cout << "WebSocket client stopped" << std::endl;
}

void WebSocketClient::workerLoop() {
    running_ = true;

    try {
        memset(&info_, 0, sizeof(info_));
        info_.port = CONTEXT_PORT_NO_LISTEN;
        info_.protocols = protocols_;
        info_.gid = -1;
        info_.uid = -1;
        info_.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info_.ka_time = 30;
        info_.ka_probes = 5;
        info_.ka_interval = 5;

        context_ = lws_create_context(&info_);
        if (!context_) {
            std::cout << "Failed to create libwebsockets context" << std::endl;
            handleError("Failed to create libwebsockets context");
            running_ = false;
            connected_ = false;
            return;
        }

        struct lws_client_connect_info ccinfo = {};
        ccinfo.context = context_;
        ccinfo.address = host_.c_str();
        ccinfo.port = port_;
        ccinfo.path = path_.c_str();
        ccinfo.host = ccinfo.address;
        ccinfo.origin = ccinfo.address;
        ccinfo.protocol = protocols_[0].name;
        ccinfo.ssl_connection = (port_ == 443) ? LCCSCF_USE_SSL : 0;

        wsi_ = lws_client_connect_via_info(&ccinfo);
        if (!wsi_) {
            std::cout << "Failed to create WebSocket connection" << std::endl;
            handleError("Failed to create WebSocket connection");
            running_ = false;
            connected_ = false;
            return;
        }

        while (running_.load()) {
            int result = lws_service(context_, 50);
            if (result < 0) {
                std::cout << "lws_service error, terminating worker loop" << std::endl;
                break;
            }
            flushTxQueue();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

    } catch (const std::exception& e) {
        std::cout << "WebSocket worker exception: " << e.what() << std::endl;
        handleError(e.what());
    }

    running_ = false;
    connected_ = false;
}

void WebSocketClient::handleConnect() {
    std::cout << "WebSocket connection established" << std::endl;
    connected_ = true;

    std::vector<std::string> subs;
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        subs.swap(pending_subscriptions_);
    }
    for (const auto& sub : subs) {
        sendMessage(sub);
        std::cout << "Sent pending subscription" << std::endl;
    }
}

void WebSocketClient::handleDisconnect() {
    std::cout << "WebSocket connection closed" << std::endl;
    connected_ = false;
}

void WebSocketClient::handleMessage(const std::string& message) {
    message_count_++;

    try {
        nlohmann::json json_msg = nlohmann::json::parse(message);

        if (message.find("error") != std::string::npos) {
            std::cout << "ERROR MESSAGE RECEIVED:" << std::endl;
            std::cout << "   Raw: " << message << std::endl;

            if (json_msg.contains("message")) {
                std::cout << "   Error message: " << json_msg["message"] << std::endl;
            }
            if (json_msg.contains("error")) {
                std::cout << "   Error field: " << json_msg["error"] << std::endl;
            }
        }

        if (message_callback_) {
            message_callback_(json_msg);
        }

    } catch (const std::exception& e) {
        std::cout << "Error parsing JSON message: " << e.what() << std::endl;
        std::cout << "Raw message: " << message.substr(0, 500) << std::endl;
        error_count_++;
    }
}

void WebSocketClient::handleError(const std::string& error) {
    error_count_++;
    std::cout << "WebSocket error: " << error << std::endl;
}

bool WebSocketClient::parseUrl(const std::string& url, std::string& host, std::string& path, int& port) {
    std::string remaining = url;

    if (remaining.find("wss://") == 0) {
        remaining = remaining.substr(6);
        port = 443;
    } else if (remaining.find("ws://") == 0) {
        remaining = remaining.substr(5);
        port = 80;
    } else {
        return false;
    }

    size_t path_pos = remaining.find('/');
    if (path_pos != std::string::npos) {
        host = remaining.substr(0, path_pos);
        path = remaining.substr(path_pos);
    } else {
        host = remaining;
        path = "/";
    }

    size_t port_pos = host.find(':');
    if (port_pos != std::string::npos) {
        port = std::stoi(host.substr(port_pos + 1));
        host = host.substr(0, port_pos);
    }

    return true;
}

int WebSocketClient::lwsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                                 void* /*user*/, void* in, size_t len) {

    if (!client_instance || !client_instance->running_) return 0;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            client_instance->handleConnect();
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (in && len > 0) {
                std::string fragment(static_cast<char*>(in), len);
                client_instance->rx_buffer_ += fragment;

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
            client_instance->wsi_ = nullptr;
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

    if (tx_queue_.empty() || !wsi_) return;

    const std::string& message = tx_queue_.front();

    size_t msg_len = message.length();
    std::vector<unsigned char> buffer(LWS_PRE + msg_len);
    memcpy(&buffer[LWS_PRE], message.c_str(), msg_len);

    int n = lws_write(wsi_, &buffer[LWS_PRE], msg_len, LWS_WRITE_TEXT);
    if (n < 0) {
        std::cout << "Failed to send WebSocket message" << std::endl;
        handleError("Failed to send message");
        return;
    }

    tx_queue_.erase(tx_queue_.begin());

    if (!tx_queue_.empty()) {
        lws_callback_on_writable(wsi_);
    }
}

std::string WebSocketClient::createJwtToken() const {
    try {
        std::string private_key = secret_key_;
        size_t pos = 0;
        while ((pos = private_key.find("\\n", pos)) != std::string::npos) {
            private_key.replace(pos, 2, "\n");
            pos += 1;
        }

        unsigned char nonce_raw[16];
        if (RAND_bytes(nonce_raw, sizeof(nonce_raw)) != 1) {
            std::cerr << "RAND_bytes failed for JWT nonce" << std::endl;
            return "";
        }
        std::string nonce(reinterpret_cast<char*>(nonce_raw), sizeof(nonce_raw));

        std::string uri = "GET api.coinbase.com";

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
        std::cerr << "WebSocket JWT token creation failed: " << e.what() << std::endl;
        return "";
    }
}
