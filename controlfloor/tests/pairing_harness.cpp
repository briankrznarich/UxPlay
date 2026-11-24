// SPDX-License-Identifier: GPL-3.0-or-later
#include "pairing.h"
#include <ixwebsocket/IXNetSystem.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using J = nlohmann::json;
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    ix::initNetSystem();
    controlfloor::PairingOptions options;
    options.helper=argv[1]; options.directory=argv[2]; options.port=static_cast<unsigned short>(std::stoi(argv[3]));
    options.promptDelay=.1; options.retryDelay=8; options.timeout=12;
    controlfloor::PairingCoordinator coordinator(options);
    coordinator.start();
    std::cout << J{{"port",coordinator.port()}}.dump() << std::endl;
    std::atomic<bool> running{true};
    std::thread output([&] {
        while (running) {
            controlfloor::PairingOutput item;
            while (coordinator.take(item))
                std::cout << J{{"worker",item.worker},{"message",item.message},{"disconnect",item.disconnect}}.dump() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::string line;
    while (std::getline(std::cin,line)) {
        auto command=J::parse(line);
        if (command.value("type", "")=="quit") break;
        coordinator.event(command.at("type"),command.at("worker"),command.at("peer"),command.value("message",J::object()).dump());
    }
    running=false; output.join(); coordinator.stop();
}
