#include "file_service.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace remydesk {

namespace {

bool pathContains(const fs::path &parent, const fs::path &child) {
    const auto mismatch = std::mismatch(parent.begin(), parent.end(), child.begin(), child.end());
    return mismatch.first == parent.end();
}

double boundedNumber(const json &value, const char *name, double minimum, double maximum) {
    if (!value.is_number()) throw std::invalid_argument(std::string("layout ") + name + " is not numeric");
    const double number = value.get<double>();
    if (!std::isfinite(number)) throw std::invalid_argument(std::string("layout ") + name + " is not finite");
    return std::clamp(number, minimum, maximum);
}

}  // namespace

FileService::FileService(const Config &config) : config_(config) {}

fs::path FileService::resolve(const std::string &relative, bool requireExists) const {
    fs::path clean;
    for (const auto &part : fs::path(relative)) {
        if (part == "." || part.empty()) continue;
        if (part == "..") throw std::invalid_argument("path escapes storage root");
        clean /= part;
    }
    fs::path value = fs::weakly_canonical(config_.storageRoot / clean);
    const fs::path root = fs::weakly_canonical(config_.storageRoot);
    const auto mismatch = std::mismatch(root.begin(), root.end(), value.begin(), value.end());
    if (mismatch.first != root.end()) throw std::invalid_argument("path escapes storage root");
    if (requireExists && !fs::exists(value)) throw std::invalid_argument("path does not exist");
    return value;
}

json FileService::list(const std::string &relative) const {
    const fs::path directory = resolve(relative);
    if (!fs::is_directory(directory)) throw std::invalid_argument("path is not a directory");
    json entries = json::array();
    for (const auto &entry : fs::directory_iterator(directory)) {
        std::error_code error;
        const bool isDirectory = entry.is_directory(error);
        const auto size = isDirectory ? 0 : entry.file_size(error);
        entries.push_back({
            {"name", entry.path().filename().string()},
            {"path", fs::relative(entry.path(), config_.storageRoot).generic_string()},
            {"type", isDirectory ? "dir" : "file"},
            {"size", error ? 0 : size},
        });
    }
    std::sort(entries.begin(), entries.end(), [](const json &left, const json &right) {
        if (left["type"] != right["type"]) return left["type"] == "dir";
        return left["name"].get<std::string>() < right["name"].get<std::string>();
    });
    return {{"path", relative}, {"entries", entries}};
}

json FileService::createDirectory(const std::string &relative, const std::string &name) const {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) {
        throw std::invalid_argument("invalid directory name");
    }
    const fs::path parent = resolve(relative);
    const fs::path target = parent / name;
    if (!fs::create_directory(target)) throw std::runtime_error("directory already exists");
    return {{"ok", true}, {"path", fs::relative(target, config_.storageRoot).generic_string()}};
}

json FileService::remove(const std::string &relative) const {
    const fs::path target = resolve(relative);
    if (target == fs::weakly_canonical(config_.storageRoot)) throw std::invalid_argument("cannot delete storage root");
    const auto removed = fs::remove_all(target);
    return {{"ok", true}, {"removed", removed}};
}

json FileService::move(const std::string &sourceRelative, const std::string &destinationRelative) const {
    const fs::path root = fs::weakly_canonical(config_.storageRoot);
    const fs::path source = resolve(sourceRelative);
    const fs::path destination = resolve(destinationRelative);
    if (source == root) throw std::invalid_argument("cannot move storage root");
    if (!fs::is_directory(destination)) throw std::invalid_argument("destination is not a directory");
    if (fs::is_directory(source) && pathContains(source, destination)) {
        throw std::invalid_argument("cannot move a directory into itself");
    }

    const fs::path target = destination / source.filename();
    if (target == source) {
        return {{"ok", true}, {"path", fs::relative(source, root).generic_string()}};
    }
    if (fs::exists(target)) throw std::invalid_argument("destination already contains an item with this name");

    std::error_code error;
    fs::rename(source, target, error);
    if (error) throw std::runtime_error("move failed: " + error.message());
    return {{"ok", true}, {"path", fs::relative(target, root).generic_string()}};
}

json FileService::writeUpload(const std::string &relative, const std::string &name,
                              const std::string &body) const {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw std::invalid_argument("invalid file name");
    }
    const fs::path directory = resolve(relative);
    const fs::path target = directory / name;
    const fs::path temporary = directory / ("." + name + ".uploading");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("cannot create upload file");
        stream.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    fs::rename(temporary, target);
    return {{"ok", true}, {"path", fs::relative(target, config_.storageRoot).generic_string()}};
}

json FileService::readNote() const {
    std::ifstream stream(config_.noteFile());
    return {{"text", stream ? std::string(std::istreambuf_iterator<char>(stream), {}) : ""}};
}

json FileService::writeNote(const std::string &text) const {
    if (text.size() > 1024 * 1024) throw std::invalid_argument("note is too large");
    std::ofstream stream(config_.noteFile(), std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot save note");
    stream << text;
    return {{"ok", true}};
}

json FileService::readDesktopLayout() const {
    std::ifstream stream(config_.desktopLayoutFile());
    if (!stream) return {{"layout", json::object()}};
    try {
        json layout;
        stream >> layout;
        return {{"layout", layout.is_object() ? layout : json::object()}};
    } catch (...) {
        return {{"layout", json::object()}};
    }
}

json FileService::writeDesktopLayout(const json &layout) const {
    if (!layout.is_object()) throw std::invalid_argument("layout must be an object");
    if (layout.size() > 10000) throw std::invalid_argument("layout contains too many entries");

    json normalized = json::object();
    for (auto item = layout.begin(); item != layout.end(); ++item) {
        if (item.key().empty() || item.key().size() > 4096 || !item.value().is_object()) {
            throw std::invalid_argument("invalid layout entry");
        }
        resolve(item.key(), false);
        json position = json::object();
        if (item.value().contains("rx")) position["rx"] = boundedNumber(item.value()["rx"], "rx", 0.0, 1.0);
        if (item.value().contains("ry")) position["ry"] = boundedNumber(item.value()["ry"], "ry", 0.0, 1.0);
        if (item.value().contains("x")) position["x"] = boundedNumber(item.value()["x"], "x", 0.0, 100000.0);
        if (item.value().contains("y")) position["y"] = boundedNumber(item.value()["y"], "y", 0.0, 100000.0);
        if (position.empty()) continue;
        normalized[item.key()] = std::move(position);
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path temporary = config_.desktopLayoutFile();
    temporary += ".tmp." + std::to_string(nonce);
    {
        std::ofstream stream(temporary, std::ios::trunc);
        if (!stream) throw std::runtime_error("cannot save desktop layout");
        stream << normalized.dump(2) << '\n';
        if (!stream) throw std::runtime_error("cannot save desktop layout");
    }
    std::error_code error;
    fs::rename(temporary, config_.desktopLayoutFile(), error);
    if (error) {
        fs::remove(temporary);
        throw std::runtime_error("cannot replace desktop layout: " + error.message());
    }
    return {{"ok", true}, {"layout", normalized}};
}

}  // namespace remydesk
