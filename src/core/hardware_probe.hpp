#pragma once

#include <filesystem>

#include "json.hpp"

namespace remydesk {

nlohmann::json probeHardware();
void writeHardwareProbe(const std::filesystem::path &path);

}  // namespace remydesk

