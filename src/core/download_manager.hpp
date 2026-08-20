#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "json.hpp"

namespace remydesk {

class DownloadManager {
public:
    explicit DownloadManager(const Config &config);
    ~DownloadManager();

    DownloadManager(const DownloadManager &) = delete;
    DownloadManager &operator=(const DownloadManager &) = delete;

    nlohmann::json enqueue(const std::string &url);
    nlohmann::json list() const;

private:
    struct Task {
        std::uint64_t id{};
        std::string url;
        std::string name;
        std::string status{"queued"};
        std::string error;
        std::filesystem::path temporary;
        std::uint64_t bytes{};
        std::int64_t createdAt{};
        std::int64_t startedAt{};
        std::int64_t finishedAt{};
    };

    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::shared_ptr<Task>> tasks_;
    std::deque<std::shared_ptr<Task>> queue_;
    std::thread worker_;
    std::atomic_bool stopping_{false};
    std::uint64_t nextId_{1};

    void work();
    void download(const std::shared_ptr<Task> &task);
    static std::string validateUrl(const std::string &url);
    static std::string filenameFromUrl(const std::string &url, std::uint64_t id);
    std::filesystem::path uniqueTarget(const std::string &name) const;
    nlohmann::json taskJson(const Task &task, std::size_t queuePosition) const;
};

}  // namespace remydesk
