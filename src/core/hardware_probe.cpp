#include "hardware_probe.hpp"

#include "process.hpp"

#include <sys/utsname.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace remydesk {

static std::string readText(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::ostringstream output;
    output << stream.rdbuf();
    std::string value = output.str();
    for (char &ch : value) {
        if (ch == '\0') ch = ',';
    }
    return trim(value);
}

static std::string packageVersion(const std::string &name) {
    const auto result = runCommand({"pkg-config", "--modversion", name}, std::chrono::seconds(3));
    return result.ok() ? trim(result.output) : "";
}

static bool toolExists(const std::string &name) {
    return runCommand({"which", name}, std::chrono::seconds(2)).ok();
}

static json drmProbe() {
    json devices = json::array();
    json preferred = {{"card", ""}, {"connector", ""}};
    const fs::path root("/sys/class/drm");
    std::error_code error;
    if (!fs::exists(root, error)) return {{"devices", devices}, {"preferred", preferred}};

    for (const auto &entry : fs::directory_iterator(root, error)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        if (name.size() <= 4 || !std::isdigit(name[4])) continue;

        json connectors = json::array();
        for (const auto &connectorEntry : fs::directory_iterator(root, error)) {
            const std::string connectorName = connectorEntry.path().filename().string();
            const std::string prefix = name + "-";
            if (connectorName.rfind(prefix, 0) != 0) continue;
            const std::string status = readText(connectorEntry.path() / "status");
            if (status.empty()) continue;
            const std::string shortName = connectorName.substr(prefix.size());
            connectors.push_back({{"name", shortName}, {"status", status}});
            if (preferred["card"].get<std::string>().empty() && status == "connected") {
                preferred = {{"card", "/dev/dri/" + name}, {"connector", shortName}};
            }
        }
        devices.push_back({{"card", "/dev/dri/" + name}, {"connectors", connectors}});
    }
    if (preferred["card"].get<std::string>().empty() && !devices.empty()) {
        preferred["card"] = devices.front()["card"];
    }
    return {{"devices", devices}, {"preferred", preferred}};
}

json probeHardware() {
    utsname system{};
    uname(&system);
    const std::string model = readText("/proc/device-tree/model");
    const std::string compatible = readText("/proc/device-tree/compatible");
    const bool rk3399 = model.find("RK3399") != std::string::npos ||
                        model.find("3399J") != std::string::npos ||
                        compatible.find("rk3399") != std::string::npos;
    const bool rk3588 = model.find("RK3588") != std::string::npos ||
                        compatible.find("rk3588") != std::string::npos;
    return {
        {"schema", 1},
        {"system", {
            {"kernel", system.release},
            {"architecture", system.machine},
            {"os_release", readText("/etc/os-release")},
        }},
        {"board", {
            {"model", model},
            {"compatible", compatible},
            {"soc", rk3399 ? "rk3399" : (rk3588 ? "rk3588" : "unknown")},
            {"rk3399", rk3399},
            {"rk3588", rk3588},
        }},
        {"drm", drmProbe()},
        {"media", {
            {"libdrm", packageVersion("libdrm")},
            {"rga", packageVersion("librga")},
            {"mpp", packageVersion("rockchip_mpp")},
            {"rknn_runtime", fs::exists("/lib/librknnrt.so") || fs::exists("/usr/lib/librknnrt.so")},
        }},
        {"tools", {
            {"nmcli", toolExists("nmcli")},
            {"loginctl", toolExists("loginctl")},
            {"xclip", toolExists("xclip")},
        }},
    };
}

void writeHardwareProbe(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path);
    if (!stream) throw std::runtime_error("cannot write hardware probe: " + path.string());
    stream << probeHardware().dump(2) << '\n';
}

}  // namespace remydesk
