#include "network_manager.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

using json = nlohmann::json;

namespace remydesk {

NetworkManager::NetworkManager(const Config &config) : config_(config) {}

NetworkManager::~NetworkManager() {
    std::lock_guard guard(workerMutex_);
    if (connectThread_.joinable()) connectThread_.join();
    if (ipv4Thread_.joinable()) ipv4Thread_.join();
}

std::vector<std::string> NetworkManager::splitTerse(const std::string &line) {
    std::vector<std::string> fields;
    std::string current;
    bool escaping = false;
    for (const char ch : line) {
        if (escaping) {
            current.push_back(ch);
            escaping = false;
        } else if (ch == '\\') {
            escaping = true;
        } else if (ch == ':') {
            fields.push_back(current);
            current.clear();
        } else if (ch != '\r' && ch != '\n') {
            current.push_back(ch);
        }
    }
    fields.push_back(current);
    return fields;
}

CommandResult NetworkManager::nmcli(const std::vector<std::string> &args,
                                    std::chrono::milliseconds timeout,
                                    bool check) const {
    std::vector<std::string> command{config_.nmcli};
    command.insert(command.end(), args.begin(), args.end());
    auto result = runCommand(command, timeout);
    const std::string lower = [&] {
        std::string value = result.output;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
        return value;
    }();
    const bool denied = lower.find("not authorized") != std::string::npos ||
                        lower.find("permission denied") != std::string::npos ||
                        lower.find("not permitted") != std::string::npos;
    if ((!result.ok() || denied) && getuid() != 0) {
        command = {"sudo", "-n", config_.nmcli};
        command.insert(command.end(), args.begin(), args.end());
        result = runCommand(command, timeout);
    }
    if (check && !result.ok()) {
        throw std::runtime_error(trim(result.output).empty() ? "nmcli failed" : trim(result.output));
    }
    return result;
}

json NetworkManager::activeConnections() const {
    const auto result = nmcli({"-t", "--escape", "yes", "-f", "NAME,UUID,TYPE,DEVICE",
                               "connection", "show", "--active"}, std::chrono::seconds(6), false);
    json items = json::array();
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = splitTerse(line);
        if (fields.size() >= 4) {
            items.push_back({{"name", fields[0]}, {"uuid", fields[1]},
                             {"type", fields[2]}, {"device", fields[3]}});
        }
    }
    return items;
}

json NetworkManager::connections() const {
    const auto result = nmcli({"-t", "--escape", "yes", "-f", "NAME,UUID,TYPE,DEVICE",
                               "connection", "show"}, std::chrono::seconds(8), false);
    json items = json::array();
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = splitTerse(line);
        if (fields.size() < 4 || (fields[2] != "802-11-wireless" && fields[2] != "802-3-ethernet")) continue;
        if (fields[0] == config_.hotspotConnection) continue;
        const std::string device = fields[3] == "--" ? "" : fields[3];
        items.push_back({
            {"name", fields[0]}, {"uuid", fields[1]}, {"type", fields[2]},
            {"kind", fields[2] == "802-11-wireless" ? "wifi" : "ethernet"},
            {"device", device}, {"active", !device.empty()},
        });
    }
    std::sort(items.begin(), items.end(), [](const json &left, const json &right) {
        if (left["active"] != right["active"]) return left["active"].get<bool>();
        return left["name"].get<std::string>() < right["name"].get<std::string>();
    });
    return items;
}

json NetworkManager::savedWifi() const {
    json saved = json::object();
    for (const auto &profile : connections()) {
        if (profile["type"] != "802-11-wireless") continue;
        const auto result = nmcli({"-g", "802-11-wireless.ssid", "connection", "show",
                                   "uuid", profile["uuid"].get<std::string>()},
                                  std::chrono::seconds(5), false);
        const std::string ssid = trim(result.output).empty() ? profile["name"].get<std::string>() : trim(result.output);
        saved[ssid] = profile;
    }
    return saved;
}

std::string NetworkManager::wifiInterface() const {
    const auto result = nmcli({"-t", "--escape", "yes", "-f", "DEVICE,TYPE", "device", "status"},
                              std::chrono::seconds(5), false);
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = splitTerse(line);
        if (fields.size() >= 2 && fields[1] == "wifi" && fields[0].rfind("p2p-", 0) != 0) return fields[0];
    }
    return {};
}

std::string NetworkManager::currentSsid() const {
    std::string activeUuid;
    std::string activeName;
    for (const auto &item : activeConnections()) {
        if (item["type"] == "802-11-wireless" && item["name"] != config_.hotspotConnection) {
            activeUuid = item["uuid"].get<std::string>();
            activeName = item["name"].get<std::string>();
            break;
        }
    }
    if (activeUuid.empty()) return {};
    const json saved = savedWifi();
    for (auto iterator = saved.begin(); iterator != saved.end(); ++iterator) {
        if (iterator.value()["uuid"] == activeUuid) return iterator.key();
    }
    return activeName;
}

std::string NetworkManager::hotspotSsid() const {
    std::string host(256, '\0');
    if (gethostname(host.data(), host.size()) != 0) host = "rk3588";
    host.resize(host.find('\0'));
    host = std::regex_replace(host, std::regex("[^A-Za-z0-9_.-]+"), "-");
    std::string value = config_.hotspotPrefix + (host.empty() ? "" : "-" + host.substr(0, 12));
    if (value.size() > 32) value.resize(32);
    return value;
}

bool NetworkManager::hotspotActive() const {
    for (const auto &item : activeConnections()) {
        if (item["name"] == config_.hotspotConnection) return true;
    }
    return false;
}

json NetworkManager::settings() const {
    std::ifstream stream(config_.networkSettingsFile());
    if (!stream) return json::object();
    try {
        return json::parse(stream);
    } catch (...) {
        return json::object();
    }
}

void NetworkManager::saveSettings(const json &value) const {
    std::ofstream stream(config_.networkSettingsFile());
    if (!stream) throw std::runtime_error("cannot save network settings");
    stream << value.dump(2) << '\n';
}

json NetworkManager::status() {
    const std::string current = currentSsid();
    if (!current.empty()) wifiConnectedThisBoot_.store(true);
    json state;
    {
        std::lock_guard guard(stateMutex_);
        state = connectState_;
    }
    const std::string requested = state.value("ssid", "");
    if (!state.value("running", false) && state.value("ok", false) && !requested.empty() && current != requested) {
        state["ok"] = false;
        state["stale"] = true;
        state["error"] = "曾成功连接 " + requested + "，但当前 Wi-Fi 已切换为 " +
                         (current.empty() ? "未连接" : current);
    }
    const json profiles = connections();
    return {
        {"available", !wifiInterface().empty()},
        {"iface", wifiInterface()},
        {"current_ssid", current},
        {"hotspot_active", hotspotActive()},
        {"hotspot_ssid", hotspotSsid()},
        {"hotspot_ip", config_.hotspotAddress.substr(0, config_.hotspotAddress.find('/'))},
        {"auto_hotspot", settings().value("auto_hotspot", config_.autoHotspot)},
        {"hotspot_suppressed_this_boot", wifiConnectedThisBoot_.load()},
        {"connect_state", state},
        {"connections", profiles},
    };
}

json NetworkManager::scan(bool deep) {
    const std::string interface = wifiInterface();
    if (interface.empty()) throw std::runtime_error("未检测到 Wi-Fi 设备");
    const bool hadHotspot = hotspotActive();
    if (deep && hadHotspot) {
        nmcli({"connection", "down", config_.hotspotConnection}, std::chrono::seconds(10), false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }

    try {
        nmcli({"radio", "wifi", "on"}, std::chrono::seconds(5), false);
        nmcli({"device", "wifi", "rescan", "ifname", interface}, std::chrono::seconds(12), false);
        std::this_thread::sleep_for(deep ? std::chrono::seconds(2) : std::chrono::milliseconds(800));
        const auto result = nmcli({"-t", "--escape", "yes", "-f", "SSID,SIGNAL,SECURITY,IN-USE",
                                   "device", "wifi", "list", "ifname", interface, "--rescan", "yes"},
                                  std::chrono::seconds(18));
        const json saved = savedWifi();
        const std::string current = currentSsid();
        std::map<std::string, json> bySsid;
        std::istringstream lines(result.output);
        std::string line;
        while (std::getline(lines, line)) {
            const auto fields = splitTerse(line);
            if (fields.size() < 4 || fields[0].empty() || fields[0] == hotspotSsid()) continue;
            int signal = 0;
            try { signal = std::stoi(fields[1]); } catch (...) {}
            json item = {{"ssid", fields[0]}, {"signal", signal}, {"security", fields[2]},
                         {"known", saved.contains(fields[0])}, {"in_use", fields[0] == current}};
            const auto found = bySsid.find(fields[0]);
            if (found == bySsid.end() || signal > found->second["signal"].get<int>()) bySsid[fields[0]] = item;
        }
        json networks = json::array();
        for (auto &[ssid, item] : bySsid) networks.push_back(item);
        std::sort(networks.begin(), networks.end(), [](const json &left, const json &right) {
            return left["signal"].get<int>() > right["signal"].get<int>();
        });
        if (deep && hadHotspot && networks.empty() &&
            settings().value("auto_hotspot", config_.autoHotspot)) {
            startHotspot();
        }
        return {{"ok", true}, {"deep", deep}, {"networks", networks}};
    } catch (...) {
        if (deep && hadHotspot && settings().value("auto_hotspot", config_.autoHotspot)) {
            try { startHotspot(); } catch (...) {}
        }
        throw;
    }
}

json NetworkManager::connectWifi(const std::string &ssidValue, const std::string &password, bool known) {
    const std::string ssid = trim(ssidValue);
    if (ssid.empty()) throw std::invalid_argument("SSID 不能为空");
    {
        std::lock_guard guard(stateMutex_);
        if (connectState_.value("running", false)) throw std::runtime_error("另一个连接操作正在进行");
        connectState_ = {{"running", true}, {"ssid", ssid}, {"stage", "queued"},
                         {"ok", false}, {"error", ""}};
    }

    std::lock_guard workerGuard(workerMutex_);
    if (connectThread_.joinable()) connectThread_.join();
    connectThread_ = std::thread([this, ssid, password, known] {
        bool ok = false;
        std::string error;
        auto setStage = [this, &ssid](const std::string &stage) {
            std::lock_guard guard(stateMutex_);
            connectState_["running"] = true;
            connectState_["ssid"] = ssid;
            connectState_["stage"] = stage;
        };
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(900));
            setStage("leaving-hotspot");
            if (hotspotActive()) {
                nmcli({"connection", "down", config_.hotspotConnection}, std::chrono::seconds(12), false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            }
            const std::string interface = wifiInterface();
            if (interface.empty()) throw std::runtime_error("未检测到 Wi-Fi 设备");
            setStage("associating");
            const json saved = savedWifi();
            if ((known || password.empty()) && saved.contains(ssid)) {
                nmcli({"connection", "up", "uuid", saved[ssid]["uuid"].get<std::string>(), "ifname", interface},
                      std::chrono::seconds(40));
            } else {
                std::vector<std::string> args{"device", "wifi", "connect", ssid, "ifname", interface};
                if (!password.empty()) {
                    args.push_back("password");
                    args.push_back(password);
                }
                nmcli(args, std::chrono::seconds(40));
            }
            const json updated = savedWifi();
            if (updated.contains(ssid)) {
                const bool hotspotAtBoot = settings().value("auto_hotspot", config_.autoHotspot);
                nmcli({"connection", "modify", "uuid", updated[ssid]["uuid"].get<std::string>(),
                       "connection.autoconnect", hotspotAtBoot ? "no" : "yes"},
                      std::chrono::seconds(8), false);
            }
            ok = currentSsid() == ssid;
            if (ok) {
                wifiConnectedThisBoot_.store(true);
                setStage("connected");
            } else {
                error = "NetworkManager 当前连接不是 " + ssid;
            }
        } catch (const std::exception &exception) {
            error = exception.what();
            setStage("manual-recovery-window");
            for (int second = 0; second < 20; ++second) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!currentSsid().empty()) {
                    ok = true;
                    error.clear();
                    wifiConnectedThisBoot_.store(true);
                    break;
                }
            }
            if (!ok && !wifiConnectedThisBoot_.load() &&
                settings().value("auto_hotspot", config_.autoHotspot)) {
                setStage("restoring-hotspot");
                try { startHotspot(); } catch (const std::exception &hotspotError) {
                    error += "; 热点恢复失败: " + std::string(hotspotError.what());
                }
            }
        }
        std::lock_guard guard(stateMutex_);
        connectState_ = {{"running", false}, {"ssid", ssid},
                         {"stage", ok ? "connected" : "failed"},
                         {"ok", ok}, {"error", error}};
    });
    return {{"ok", true}, {"connecting", true}, {"ssid", ssid}};
}

json NetworkManager::startHotspot() {
    if (config_.hotspotPassword.size() < 8) throw std::runtime_error("热点密码至少需要 8 个字符");
    if (hotspotActive()) return status();
    const std::string interface = wifiInterface();
    if (interface.empty()) throw std::runtime_error("未检测到 Wi-Fi 设备");
    const auto existing = nmcli({"connection", "show", config_.hotspotConnection},
                                std::chrono::seconds(6), false);
    if (!existing.ok()) {
        nmcli({"connection", "add", "type", "wifi", "ifname", interface,
               "con-name", config_.hotspotConnection, "ssid", hotspotSsid()},
              std::chrono::seconds(12));
    }
    nmcli({"connection", "modify", config_.hotspotConnection, "ipv4.method", "shared",
           "ipv4.addresses", config_.hotspotAddress, "ipv6.method", "ignore",
           "connection.autoconnect", "no", "802-11-wireless.mode", "ap",
           "802-11-wireless.ssid", hotspotSsid(), "wifi-sec.key-mgmt", "wpa-psk",
           "wifi-sec.psk", config_.hotspotPassword}, std::chrono::seconds(12));
    nmcli({"connection", "up", config_.hotspotConnection}, std::chrono::seconds(20));
    json value = status();
    value["ok"] = true;
    return value;
}

json NetworkManager::stopHotspot() {
    nmcli({"connection", "down", config_.hotspotConnection}, std::chrono::seconds(10), false);
    json value = status();
    value["ok"] = true;
    return value;
}

json NetworkManager::setAutoHotspot(bool enabled) {
    json value = settings();
    value["auto_hotspot"] = enabled;
    saveSettings(value);
    for (const auto &profile : connections()) {
        if (profile["type"] != "802-11-wireless" ||
            profile["name"] == config_.hotspotConnection) continue;
        nmcli({"connection", "modify", "uuid", profile["uuid"].get<std::string>(),
               "connection.autoconnect", enabled ? "no" : "yes"},
              std::chrono::seconds(8), false);
    }
    return {{"ok", true}, {"auto_hotspot", enabled},
            {"boot_mode", enabled ? "hotspot" : "saved-wifi"}};
}

json NetworkManager::ipv4(const std::string &key) {
    json profile;
    for (const auto &item : connections()) {
        if (item["uuid"] == key || item["name"] == key) {
            profile = item;
            break;
        }
    }
    if (profile.is_null()) return {{"uuid", ""}, {"mode", "auto"}};
    const auto result = nmcli({"-t", "--escape", "yes", "-f",
                               "ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns",
                               "connection", "show", "uuid", profile["uuid"].get<std::string>()},
                              std::chrono::seconds(7), false);
    std::map<std::string, std::string> values;
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = splitTerse(line);
        if (fields.size() >= 2) values[fields[0]] = fields[1];
    }
    profile["mode"] = values["ipv4.method"] == "manual" ? "manual" : "auto";
    profile["address"] = values["ipv4.addresses"];
    profile["gateway"] = values["ipv4.gateway"];
    profile["dns"] = values["ipv4.dns"];
    return profile;
}

json NetworkManager::setIpv4(const json &request) {
    const std::string key = request.value("connection", "");
    json profile;
    for (const auto &item : connections()) {
        if (item["uuid"] == key || item["name"] == key) { profile = item; break; }
    }
    if (profile.is_null()) throw std::invalid_argument("请选择网络连接");
    const std::string mode = request.value("mode", "auto");
    std::vector<std::string> args{"connection", "modify", "uuid", profile["uuid"].get<std::string>()};
    if (mode == "manual") {
        const std::string address = request.value("address", "");
        if (address.find('/') == std::string::npos) throw std::invalid_argument("静态地址需要包含前缀，例如 /24");
        args.insert(args.end(), {"ipv4.method", "manual", "ipv4.addresses", address,
                                 "ipv4.gateway", request.value("gateway", ""),
                                 "ipv4.dns", request.value("dns", ""), "ipv4.ignore-auto-dns", "yes"});
    } else {
        args.insert(args.end(), {"ipv4.method", "auto", "ipv4.addresses", "", "ipv4.gateway", "",
                                 "ipv4.dns", "", "ipv4.ignore-auto-dns", "no"});
    }
    nmcli(args, std::chrono::seconds(12));
    if (profile.value("active", false)) {
        const std::string uuid = profile["uuid"];
        std::lock_guard workerGuard(workerMutex_);
        if (ipv4Thread_.joinable()) ipv4Thread_.join();
        ipv4Thread_ = std::thread([this, uuid] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            nmcli({"connection", "up", "uuid", uuid}, std::chrono::seconds(40), false);
        });
    }
    return {{"ok", true}, {"message", "IPv4 设置已保存"}};
}

void NetworkManager::maybeStartHotspot() {
    if (!settings().value("auto_hotspot", config_.autoHotspot) ||
        wifiConnectedThisBoot_.load() || !currentSsid().empty() || hotspotActive()) return;
    try { startHotspot(); } catch (const std::exception &exception) {
        std::cerr << "RemyDesk hotspot start failed: " << exception.what() << '\n';
    }
}

}  // namespace remydesk
