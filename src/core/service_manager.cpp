#include "service_manager.hpp"

#include "process.hpp"

#include <unistd.h>

#include <stdexcept>

using json = nlohmann::json;

namespace remydesk {

ServiceManager::ServiceManager(const Config &config) : config_(config) {}

CommandResult ServiceManager::systemctl(const std::vector<std::string> &args, bool check) const {
    std::vector<std::string> command{config_.systemctl};
    command.insert(command.end(), args.begin(), args.end());
    auto result = runCommand(command, std::chrono::seconds(15));
    // Read-only queries such as is-active intentionally return non-zero for
    // inactive/static units while still printing the state we need. Retrying
    // those queries through sudo creates noisy authentication failures and is
    // unnecessary. Mutating operations call this helper with check=true.
    if (check && !result.ok() && getuid() != 0) {
        command = {"sudo", "-n", config_.systemctl};
        command.insert(command.end(), args.begin(), args.end());
        result = runCommand(command, std::chrono::seconds(15));
    }
    if (check && !result.ok()) {
        throw std::runtime_error(trim(result.output).empty() ? "systemctl failed" : trim(result.output));
    }
    return result;
}

json ServiceManager::desktopStatus(const std::string &host) const {
    const std::string active = trim(systemctl({"is-active", config_.desktopService}, false).output);
    const std::string enabled = trim(systemctl({"is-enabled", config_.desktopService}, false).output);
    return {
        {"service", config_.desktopService},
        {"active", active},
        {"enabled", enabled},
        {"running", active == "active"},
        {"autostart", enabled == "enabled"},
        {"url", "http://" + host + ":" + std::to_string(config_.desktopPort) + "/"},
    };
}

json ServiceManager::switchDesktop(bool enabled, const std::string &host) const {
    systemctl({enabled ? "start" : "stop", config_.desktopService});
    json result = desktopStatus(host);
    result["ok"] = true;
    return result;
}

json ServiceManager::poweroff() const {
    systemctl({"--no-ask-password", "poweroff"});
    return {{"ok", true}};
}

}  // namespace remydesk
