#pragma once

#include <filesystem>
#include <string>

namespace remydesk {

struct Config {
    std::string host{"0.0.0.0"};
    int port{8010};
    std::filesystem::path storageRoot;
    std::filesystem::path stateDir{"/var/lib/remydesk"};
    std::filesystem::path runtimeDir{"/run/remydesk"};
    std::filesystem::path hardwareFile{"/var/lib/remydesk/hardware.json"};
    std::filesystem::path webRoot{"/opt/remydesk/share/web"};
    std::string authUser{"remy"};
    std::string authPassword;
    std::string desktopService{"remydesk-desktop.service"};
    int desktopPort{8088};
    std::string nmcli{"/usr/bin/nmcli"};
    std::string systemctl{"/usr/bin/systemctl"};
    bool autoHotspot{true};
    std::string hotspotConnection{"remydesk-setup"};
    std::string hotspotPrefix{"RemyDesk-Setup"};
    std::string hotspotPassword{"12345678"};
    std::string hotspotAddress{"192.168.12.58/24"};

    static Config fromEnvironment();
    void prepare();

    [[nodiscard]] std::filesystem::path noteFile() const;
    [[nodiscard]] std::filesystem::path desktopLayoutFile() const;
    [[nodiscard]] std::filesystem::path networkSettingsFile() const;
    [[nodiscard]] std::filesystem::path scanCacheFile() const;
};

std::string environment(const char *name, const std::string &fallback = {});
bool environmentBool(const char *name, bool fallback);

}  // namespace remydesk
