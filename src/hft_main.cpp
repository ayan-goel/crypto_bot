#include "hft_engine.h"
#include "config.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <iomanip>
#include <algorithm>

// Global flag for graceful shutdown
std::atomic<bool> running{true};

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ". Shutting down HFT engine..." << std::endl;
    running = false;
}

int main(int argc, char* argv[]) {
    // Set up signal handling for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    std::cout << "🚀 ================================== 🚀" << std::endl;
    std::cout << "🚀    ULTRA HIGH-FREQUENCY TRADING    🚀" << std::endl;
    std::cout << "🚀           ENGINE v2.0              🚀" << std::endl;
    std::cout << "🚀 ================================== 🚀" << std::endl;
    
    try {
        // Initialize HFT engine
        HFTEngine engine;
        
        std::string config_file = (argc > 1) ? argv[1] : "config.txt";
        if (!engine.initialize(config_file)) {
            std::cerr << "❌ Failed to initialize HFT engine" << std::endl;
            return 1;
        }
        
        // Load config to display current settings
        Config& config = Config::getInstance();
        
        std::cout << "\n⚙️  HFT Engine Configuration (from " << config_file << "):" << std::endl;
        std::cout << "   Initial Capital: $" << config.getConfig("INITIAL_CAPITAL", "50.0") << std::endl;
        std::cout << "   Spread Threshold: " << config.getSpreadThresholdBps() << " bps" << std::endl;
        std::cout << "   Order Size: " << config.getOrderSize() << " ETH" << std::endl;
        std::cout << "   Max Inventory: " << config.getMaxInventory() << " ETH" << std::endl;
        std::cout << "   Order Rate Limit: " << config.getOrderRateLimit() << " orders/sec" << std::endl;
        std::cout << "   Position Limit: " << config.getConfig("POSITION_LIMIT_ETHUSDT", "0.1") << " ETH" << std::endl;
        std::cout << "   Daily Loss Limit: $" << config.getMaxDailyLossLimit() << std::endl;
        std::cout << "   Paper Trading: " << (config.isPaperTrading() ? "ENABLED" : "DISABLED") << std::endl;
        
        std::cout << "✅ HFT engine configured using config.txt values" << std::endl;
        
        // Start the engine
        std::cout << "\n🔥 Starting HFT engine..." << std::endl;
        engine.start();
        
        std::cout << "\n📊 HFT ENGINE RUNNING - Press Ctrl+C to stop" << std::endl;
        std::cout << "📈 Target: " << config.getOrderRateLimit() << "+ orders/second with microsecond latency" << std::endl;
        std::cout << "⚡ Real-time performance metrics will be displayed every 5 seconds" << std::endl;
        std::cout << "💰 Trading with $" << config.getConfig("INITIAL_CAPITAL", "50.0") << " capital allocation" << std::endl;
        std::cout << "\n" << std::string(60, '=') << std::endl;
        
        // Performance monitoring loop
        auto start_time = std::chrono::steady_clock::now();
        
        while (running && engine.is_running()) {
            // Just wait - all metrics are handled by HFT engine using Order Manager data
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Graceful shutdown
        std::cout << "\n🛑 Initiating graceful shutdown..." << std::endl;
        auto shutdown_start = std::chrono::steady_clock::now();
        
        engine.stop();
        
        auto shutdown_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shutdown_start).count();
        
        std::cout << "✅ HFT engine stopped gracefully in " << shutdown_time << "ms" << std::endl;
        
        // Session summary will be handled by Order Manager - no duplicate summary needed here
        
    } catch (const std::exception& e) {
        std::cerr << "💥 Fatal error in HFT engine: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "🏁 HFT Engine session completed successfully" << std::endl;
    return 0;
} 