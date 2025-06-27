#include "config.h"
#include "logger.h"
#include "order_book.h"
#include "strategy.h"
#include "order_manager.h"
#include "websocket_client.h"
#include "rest_client.h"
#include "risk_manager.h"

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

// Global flag for graceful shutdown
std::atomic<bool> running{true};
std::atomic<int> signal_count{0};
std::atomic<bool> shutdown_in_progress{false};

void signalHandler(int signum) {
    if (!shutdown_in_progress.load()) {
        running = false;
        shutdown_in_progress = true;
        std::cout << "\nReceived signal " << signum << ". Shutting down gracefully..." << std::endl;
        std::cout << "(Press Ctrl+C again to force quit if shutdown hangs)" << std::endl;
    } else {
        std::cout << "\nForce quit requested. Exiting immediately..." << std::endl;
        std::exit(1);
    }
}

int main(int argc, char* argv[]) {
    // Set up signal handling for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    std::cout << "=== Crypto HFT Bot Starting ===" << std::endl;
    
    try {
        // Initialize configuration
        Config& config = Config::getInstance();
        std::string config_file = (argc > 1) ? argv[1] : "config.txt";
        
        if (!config.loadFromFile(config_file)) {
            std::cerr << "Failed to load configuration from " << config_file << std::endl;
            std::cerr << "Please ensure the config file exists and is properly formatted." << std::endl;
            return 1;
        }
        
        // Initialize logger
        Logger& logger = Logger::getInstance();
        if (!logger.initialize()) {
            std::cerr << "Failed to initialize logger" << std::endl;
            return 1;
        }
        
        logger.setLogLevel(config.getLogLevel());
        logger.setConsoleOutput(config.isLogToConsole());
        logger.setFileOutput(config.isLogToFile());
        
        logger.info("Configuration loaded successfully");
        logger.info("Trading Symbol: " + config.getTradingSymbol());
        logger.info("Environment: " + std::string(config.isTestnet() ? "TESTNET" : "MAINNET"));
        logger.info("Paper Trading: " + std::string(config.isPaperTrading() ? "ENABLED" : "DISABLED"));
        
        // Initialize components
        std::string symbol = config.getTradingSymbol();
        
        // Create core components
        OrderBook order_book(symbol);
        Strategy strategy(symbol);
        OrderManager order_manager;
        RiskManager risk_manager;
        WebSocketClient ws_client;
        RestClient rest_client;
        
        // Configure strategy parameters from config
        strategy.setSpreadThreshold(config.getSpreadThresholdBps());
        strategy.setOrderSize(config.getOrderSize());
        strategy.setMaxInventory(config.getMaxInventory());
        
        logger.info("Core components created");
        
        // Initialize risk manager
        if (!risk_manager.initialize(config_file)) {
            logger.error("Failed to initialize risk manager");
            return 1;
        }
        
        // Initialize order manager
        if (!order_manager.initialize()) {
            logger.error("Failed to initialize order manager");
            return 1;
        }
        
        // Set risk manager in order manager for PnL tracking
        order_manager.setRiskManager(&risk_manager);
        
        // Initialize REST client
        if (!rest_client.initialize()) {
            logger.error("Failed to initialize REST client");
            return 1;
        }
        
            rest_client.setApiCredentials(config.getCoinbaseApiKey(), config.getCoinbaseSecretKey());
    rest_client.setBaseUrl(config.getCoinbaseBaseUrl());
    
    // Set WebSocket API credentials for JWT authentication (Advanced Trade API)
    std::string api_key = config.getCoinbaseApiKey();
    std::string secret_key = config.getCoinbaseSecretKey();
    
    std::cout << "🔐 Setting WebSocket credentials (Advanced Trade API):" << std::endl;
    std::cout << "   API Key: " << (api_key.empty() ? "EMPTY" : api_key.substr(0, 8) + "...") << std::endl;
    std::cout << "   Secret Key: " << (secret_key.empty() ? "EMPTY" : "SET (" + std::to_string(secret_key.length()) + " chars)") << std::endl;
    
    ws_client.setApiCredentials(api_key, secret_key);
        
        // Test API connectivity
        auto ping_response = rest_client.ping();
        if (!ping_response.success) {
            logger.error("Failed to ping Coinbase API: " + ping_response.error_message);
            return 1;
        }
        
        logger.info("API connectivity test successful");
        
        // Start latency monitoring
        order_manager.startLatencyMonitoring();
        
        // Perform initial network latency test
        std::cout << "🔄 Testing network latency to Coinbase..." << std::endl;
        double network_latency = order_manager.measureNetworkLatency();
        if (network_latency > 0) {
            std::cout << "✅ Initial latency test complete" << std::endl;
            
            // Latency performance assessment
            if (network_latency < 10.0) {
                std::cout << "🚀 EXCELLENT latency (<10ms) - Optimal for HFT" << std::endl;
            } else if (network_latency < 50.0) {
                std::cout << "✅ GOOD latency (<50ms) - Suitable for HFT" << std::endl;
            } else if (network_latency < 100.0) {
                std::cout << "⚠️  MODERATE latency (<100ms) - May impact HFT performance" << std::endl;
            } else {
                std::cout << "🐌 HIGH latency (>100ms) - Consider co-location for better performance" << std::endl;
            }
        }
        
        // Set up WebSocket callbacks
        ws_client.setMessageCallback([&](const nlohmann::json& message) {
            try {
                // Update order book with incoming market data
                if (order_book.updateFromWebSocket(message)) {
                    auto snapshot = order_book.getSnapshot();
                    
                    if (snapshot.is_valid) {
                        // Log order book update
                        logger.logOrderBook(snapshot.symbol, 
                                          snapshot.best_bid_price, 
                                          snapshot.best_ask_price,
                                          snapshot.best_bid_quantity, 
                                          snapshot.best_ask_quantity);
                        
                        // Generate trading signal
                        auto signal = strategy.generateSignal(snapshot);
                        
                        // Debug: Always log the signal reason
                        std::cout << "📈 SIGNAL: " << signal.reason 
                                  << " (Spread: " << snapshot.spread_bps << " bps)" << std::endl;
                        
                        // Process signal - now with risk management!
                        if (signal.should_place_bid || signal.should_place_ask) {
                            logger.info("Trading signal generated: " + signal.reason);
                            
                            // Execute orders through order manager with risk checks
                            if (signal.should_place_bid) {
                                std::string rejection_reason;
                                if (risk_manager.canPlaceOrder(symbol, "BUY", signal.bid_price, signal.bid_quantity, rejection_reason)) {
                                    auto future_response = order_manager.placeOrder(symbol, "BUY", signal.bid_price, signal.bid_quantity);
                                    auto response = future_response.get();
                                    if (response.success) {
                                        risk_manager.updatePosition(symbol, signal.bid_quantity, signal.bid_price, "BUY");
                                        risk_manager.recordOrderPlaced();
                                    }
                                } else {
                                    logger.warning("BUY order rejected by risk manager: " + rejection_reason);
                                }
                            }
                            
                            if (signal.should_place_ask) {
                                std::string rejection_reason;
                                if (risk_manager.canPlaceOrder(symbol, "SELL", signal.ask_price, signal.ask_quantity, rejection_reason)) {
                                    auto future_response = order_manager.placeOrder(symbol, "SELL", signal.ask_price, signal.ask_quantity);
                                    auto response = future_response.get();
                                    if (response.success) {
                                        risk_manager.updatePosition(symbol, signal.ask_quantity, signal.ask_price, "SELL");
                                        risk_manager.recordOrderPlaced();
                                    }
                                } else {
                                    logger.warning("SELL order rejected by risk manager: " + rejection_reason);
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                logger.error("Error in market data callback: " + std::string(e.what()));
            }
        });
        
        ws_client.setConnectionCallback([&](bool connected) {
            if (connected) {
                logger.info("WebSocket connected successfully");
            } else {
                logger.warning("WebSocket disconnected");
            }
        });
        
        ws_client.setErrorCallback([&](const std::string& error) {
            logger.error("WebSocket error: " + error);
        });
        
        // Connect to WebSocket
        if (!ws_client.connect(config.getCoinbaseWsUrl())) {
            logger.error("Failed to connect to WebSocket");
            return 1;
        }
        
        // Subscribe to order book updates
        if (!ws_client.subscribeOrderBook(symbol, config.getOrderbookDepth())) {
            logger.error("Failed to subscribe to order book updates");
            return 1;
        }
        
        ws_client.enablePing(config.getWebsocketPingInterval());
        
        // Start risk monitoring
        risk_manager.startRiskMonitoring();
        
        logger.info("Bot initialized and running...");
        logger.info("Press Ctrl+C to stop");
        
        // Main loop
        auto last_health_check = std::chrono::steady_clock::now();
        auto last_stats_print = std::chrono::steady_clock::now();
        auto shutdown_start = std::chrono::steady_clock::now();
        const auto health_check_interval = std::chrono::seconds(30);
        const auto stats_print_interval = std::chrono::minutes(5);
        const auto shutdown_timeout = std::chrono::seconds(5);
        
        while (running) {
            auto now = std::chrono::steady_clock::now();
            
            // Periodic health checks
            if (now - last_health_check >= health_check_interval) {
                bool healthy = ws_client.isHealthy() && 
                              rest_client.isHealthy() && 
                              order_manager.isHealthy();
                
                logger.logHealth("system", healthy, 
                               healthy ? "All components healthy" : "Some components unhealthy");
                
                last_health_check = now;
            }
            
            // Periodic statistics
            if (now - last_stats_print >= stats_print_interval) {
                logger.info("=== System Statistics ===");
                ws_client.printStats();
                rest_client.printStats();
                order_manager.printStats();
                strategy.printStats();
                
                // Print risk summary
                std::cout << risk_manager.getRiskSummary() << std::endl;
                
                last_stats_print = now;
            }
            
            // Check order statuses periodically
            order_manager.checkOrderStatuses();
            order_manager.cleanupExpiredOrders();
            
            // Sleep very briefly for HFT - faster reaction time
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        // Graceful shutdown
        logger.info("Initiating graceful shutdown...");
        auto graceful_shutdown_start = std::chrono::steady_clock::now();
        
        // Print final latency statistics
        std::cout << "\n🔄 Final latency performance report:" << std::endl;
        order_manager.printLatencyStats();
        order_manager.stopLatencyMonitoring();

        ws_client.stop();
        ws_client.disconnect();

        // Check for shutdown timeout
        if (std::chrono::steady_clock::now() - graceful_shutdown_start > shutdown_timeout) {
            std::cout << "Shutdown timeout reached. Force exiting..." << std::endl;
            std::exit(1);
        }
        
        // Generate final reports after all trading threads stopped
        order_manager.shutdown();
        risk_manager.shutdown();
        
        if (!config.isPaperTrading()) {
            // Cancel all open orders before shutdown
            logger.info("Canceling all open orders...");
            // TODO: Implement order cancellation
        }
        
        rest_client.cleanup();
        
        logger.info("Shutdown complete");
        logger.flush();
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "=== Crypto HFT Bot Stopped ===" << std::endl;
    return 0;
} 