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
    
    std::cout << "🚀 HFT Engine v2.0" << std::endl;
    
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
        
        // Configuration loaded silently
        
        // Start the engine
        engine.start();
        
        std::cout << "📊 Trading active - Press Ctrl+C to stop" << std::endl;
        
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