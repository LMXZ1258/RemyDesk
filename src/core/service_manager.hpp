#pragma once

#include <string>
#include <vector>

#include "config.hpp"
#include "json.hpp"
#include "process.hpp"

namespace remydesk {

class ServiceManager {
public:
    explicit ServiceManager(const Config &config);

    nlohmann::json desktopStatus(const std::string &host) const;
    nlohmann::json switchDesktop(bool enabled, const std::string &host) const;
    nlohmann::json poweroff() const;

private:
    Config config_;
    CommandResult systemctl(const std::vector<std::string> &args, bool check = true) const;
};

}  // namespace remydesk
