#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

#include "config.hpp"
#include "hardware_probe.hpp"
#include "http_server.hpp"

namespace {
remydesk::HttpServer *server = nullptr;
void stopServer(int) {
    if (server) server->stop();
}
}

int main(int argc, char **argv) {
    try {
        remydesk::Config config = remydesk::Config::fromEnvironment();
        bool probeOnly = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--probe") probeOnly = true;
            else if (argument == "--help") {
                std::cout << "Usage: remydeskd [--probe]\n";
                return 0;
            } else {
                throw std::invalid_argument("unknown argument: " + argument);
            }
        }
        if (probeOnly) {
            std::cout << remydesk::probeHardware().dump(2) << '\n';
            return 0;
        }
        config.prepare();
        try { remydesk::writeHardwareProbe(config.hardwareFile); }
        catch (const std::exception &exception) { std::cerr << "hardware probe warning: " << exception.what() << '\n'; }

        remydesk::HttpServer application(std::move(config));
        server = &application;
        std::signal(SIGINT, stopServer);
        std::signal(SIGTERM, stopServer);
        if (!application.listen()) return 1;
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "ERROR: " << exception.what() << '\n';
        return 1;
    }
}
