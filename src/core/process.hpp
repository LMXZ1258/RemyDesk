#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace remydesk {

struct CommandResult {
    int exitCode{-1};
    bool timedOut{false};
    std::string output;

    [[nodiscard]] bool ok() const { return exitCode == 0 && !timedOut; }
};

CommandResult runCommand(
    const std::vector<std::string> &arguments,
    std::chrono::milliseconds timeout = std::chrono::seconds(15));

std::string trim(std::string value);

}  // namespace remydesk

