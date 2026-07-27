#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "json.hpp"
#include "process.hpp"

namespace remydesk {

class NetworkManager {
public:
    explicit NetworkManager(const Config &config);
    ~NetworkManager();

    nlohmann::json status();
    nlohmann::json scan(bool deep);
    nlohmann::json connectWifi(const std::string &ssid, const std::string &password, bool known);
    nlohmann::json startHotspot();
    nlohmann::json stopHotspot();
    nlohmann::json setAutoHotspot(bool enabled);
    nlohmann::json ipv4(const std::string &key);
    nlohmann::json setIpv4(const nlohmann::json &request);
    void maybeStartHotspot();

private:
    Config config_;
    std::mutex stateMutex_;
    std::mutex workerMutex_;
    std::thread connectThread_;
    std::thread ipv4Thread_;
    std::atomic_bool wifiConnectedThisBoot_{false};
    nlohmann::json connectState_{{"running", false}, {"ssid", ""}, {"stage", "idle"},
                                  {"ok", false}, {"error", ""}};

    CommandResult nmcli(const std::vector<std::string> &args,
                        std::chrono::milliseconds timeout = std::chrono::seconds(15),
                        bool check = true) const;
    static std::vector<std::string> splitTerse(const std::string &line);
    nlohmann::json activeConnections() const;
    nlohmann::json connections() const;
    nlohmann::json savedWifi() const;
    std::string wifiInterface() const;
    std::string currentSsid() const;
    std::string hotspotSsid() const;
    bool hotspotActive() const;
    nlohmann::json settings() const;
    void saveSettings(const nlohmann::json &settings) const;
};

}  // namespace remydesk
