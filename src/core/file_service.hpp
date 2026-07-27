#pragma once

#include <filesystem>
#include <string>

#include "config.hpp"
#include "json.hpp"

namespace remydesk {

class FileService {
public:
    explicit FileService(const Config &config);

    nlohmann::json list(const std::string &relative) const;
    nlohmann::json createDirectory(const std::string &relative, const std::string &name) const;
    nlohmann::json remove(const std::string &relative) const;
    nlohmann::json move(const std::string &source, const std::string &destination) const;
    nlohmann::json writeUpload(const std::string &relative, const std::string &name,
                               const std::string &body) const;
    nlohmann::json readNote() const;
    nlohmann::json writeNote(const std::string &text) const;
    nlohmann::json readDesktopLayout() const;
    nlohmann::json writeDesktopLayout(const nlohmann::json &layout) const;
    std::filesystem::path resolve(const std::string &relative, bool requireExists = true) const;

private:
    Config config_;
};

}  // namespace remydesk
