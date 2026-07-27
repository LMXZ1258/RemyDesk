#include "config.hpp"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <cctype>
#include <stdexcept>

namespace remydesk {

std::string environment(const char *name, const std::string &fallback) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool environmentBool(const char *name, bool fallback) {
    std::string value = environment(name);
    if (value.empty()) return fallback;
    for (char &ch : value) ch = static_cast<char>(std::tolower(ch));
    return value == "1" || value == "true" || value == "yes" || value == "on" || value == "enabled";
}

static std::filesystem::path defaultStorageRoot() {
    const std::string configuredUser = environment("REMYDESK_DESKTOP_USER", environment("SUDO_USER"));
    if (!configuredUser.empty() && configuredUser != "root") {
        if (passwd *entry = getpwnam(configuredUser.c_str())) {
            return std::filesystem::path(entry->pw_dir) / "RemyDesk";
        }
    }
    if (passwd *entry = getpwuid(getuid())) {
        return std::filesystem::path(entry->pw_dir) / "RemyDesk";
    }
    return "/srv/remydesk";
}

Config Config::fromEnvironment() {
    Config config;
    config.host = environment("REMYDESK_HOST", config.host);
    config.port = std::stoi(environment("REMYDESK_PORT", std::to_string(config.port)));
    config.storageRoot = environment("REMYDESK_STORAGE_ROOT", defaultStorageRoot().string());
    config.stateDir = environment("REMYDESK_STATE_DIR", config.stateDir.string());
    config.runtimeDir = environment("REMYDESK_RUNTIME_DIR", config.runtimeDir.string());
    config.hardwareFile = environment("REMYDESK_HARDWARE_FILE", config.hardwareFile.string());
    config.webRoot = environment("REMYDESK_WEB_ROOT", config.webRoot.string());
    config.authUser = environment("REMYDESK_AUTH_USER", config.authUser);
    config.authPassword = environment("REMYDESK_AUTH_PASSWORD");
    config.desktopService = environment("REMYDESK_DESKTOP_SERVICE", config.desktopService);
    config.desktopPort = std::stoi(environment("REMYDESK_DESKTOP_PORT", std::to_string(config.desktopPort)));
    config.nmcli = environment("REMYDESK_NMCLI", config.nmcli);
    config.systemctl = environment("REMYDESK_SYSTEMCTL", config.systemctl);
    config.autoHotspot = environmentBool("REMYDESK_AUTO_HOTSPOT", config.autoHotspot);
    config.hotspotConnection = environment("REMYDESK_HOTSPOT_CONNECTION", config.hotspotConnection);
    config.hotspotPrefix = environment("REMYDESK_HOTSPOT_PREFIX", config.hotspotPrefix);
    config.hotspotPassword = environment("REMYDESK_HOTSPOT_PASSWORD", config.hotspotPassword);
    config.hotspotAddress = environment("REMYDESK_HOTSPOT_ADDRESS", config.hotspotAddress);
    return config;
}

void Config::prepare() {
    std::filesystem::create_directories(storageRoot);
    std::filesystem::create_directories(stateDir);
    std::error_code error;
    std::filesystem::create_directories(runtimeDir, error);
    if (error) {
        runtimeDir = std::filesystem::path("/tmp") / ("remydesk-" + std::to_string(getuid()));
        std::filesystem::create_directories(runtimeDir);
    }
}

std::filesystem::path Config::noteFile() const { return stateDir / "note.txt"; }
std::filesystem::path Config::desktopLayoutFile() const { return stateDir / "desktop-layout.json"; }
std::filesystem::path Config::networkSettingsFile() const { return stateDir / "network-settings.json"; }
std::filesystem::path Config::scanCacheFile() const { return stateDir / "wifi-scan.json"; }

}  // namespace remydesk
