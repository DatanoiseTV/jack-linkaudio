#include "Bridge.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>

std::atomic<bool> gQuit(false);

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        gQuit = true;
    }
}

int main(int argc, char** argv) {
    std::string name = "jack-linkaudio";
    int numInputs = 2;
    bool sync = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--sync") {
            sync = true;
        } else if (i == 1) {
            name = arg;
        } else if (i == 2) {
            numInputs = std::stoi(arg);
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        ableton::linkaudio::Bridge bridge(name, numInputs, sync);
        bridge.start();

        std::cout << "Started bridge '" << name << "' with " << numInputs << " inputs." << std::endl;
        if (sync) {
            std::cout << "JACK transport sync is enabled." << std::endl;
        }
        std::cout << "Press Ctrl+C to stop." << std::endl;

        while (!gQuit && bridge.isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Stopping bridge..." << std::endl;
        bridge.stop();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
