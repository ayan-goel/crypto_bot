#include "engine.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>

static volatile std::sig_atomic_t signal_received = 0;

void signalHandler(int /*signum*/) {
    signal_received = 1;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "HFT Engine v2.0" << std::endl;

    try {
        HFTEngine engine;

        std::string config_file = (argc > 1) ? argv[1] : "config.txt";
        if (!engine.initialize(config_file)) {
            std::cerr << "Failed to initialize HFT engine" << std::endl;
            return 1;
        }

        engine.start();

        std::cout << "Trading active - Press Ctrl+C to stop" << std::endl;

        while (!signal_received && engine.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nInitiating graceful shutdown..." << std::endl;
        auto shutdown_start = std::chrono::steady_clock::now();

        engine.stop();

        auto shutdown_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shutdown_start).count();

        std::cout << "HFT engine stopped gracefully in " << shutdown_time << "ms" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error in HFT engine: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "HFT Engine session completed successfully" << std::endl;
    return 0;
}
