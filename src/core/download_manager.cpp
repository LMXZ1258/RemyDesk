#include "download_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "process.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace remydesk {
namespace {

std::int64_t nowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

std::string percentDecode(std::string value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

bool validUtf8(const std::string &value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        std::size_t continuation = 0;
        if (first < 0x80) continuation = 0;
        else if (first >= 0xc2 && first <= 0xdf) continuation = 1;
        else if (first >= 0xe0 && first <= 0xef) continuation = 2;
        else if (first >= 0xf0 && first <= 0xf4) continuation = 3;
        else return false;
        if (i + continuation >= value.size()) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            if ((static_cast<unsigned char>(value[i + offset]) & 0xc0) != 0x80) return false;
        }
        if (continuation >= 2) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
                (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) return false;
        }
        i += continuation + 1;
    }
    return true;
}

}  // namespace

DownloadManager::DownloadManager(const Config &config) : config_(config) {
    worker_ = std::thread([this] { work(); });
}

DownloadManager::~DownloadManager() {
    stopping_.store(true);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &task : tasks_) {
        if (!task->temporary.empty()) {
            std::error_code ignored;
            fs::remove(task->temporary, ignored);
        }
    }
}

std::string DownloadManager::validateUrl(const std::string &url) {
    if (url.empty() || url.size() > 4096) throw std::invalid_argument("下载链接为空或过长");
    std::string lower;
    lower.reserve(std::min<std::size_t>(url.size(), 8));
    for (std::size_t i = 0; i < std::min<std::size_t>(url.size(), 8); ++i) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(url[i]))));
    }
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) {
        throw std::invalid_argument("只允许 http:// 或 https:// 下载链接");
    }
    for (unsigned char value : url) {
        if (value < 0x20 || value == 0x7f) throw std::invalid_argument("下载链接包含控制字符");
    }
    return url;
}

std::string DownloadManager::filenameFromUrl(const std::string &url, std::uint64_t id) {
    std::string path = url.substr(0, url.find_first_of("?#"));
    const auto slash = path.find_last_of('/');
    std::string name = percentDecode(slash == std::string::npos ? path : path.substr(slash + 1));
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char value) {
        return value < 0x20 || value == 0x7f || value == '/' || value == '\\';
    }), name.end());
    if (!validUtf8(name) || name.empty() || name == "." || name == "..") {
        name = "download-" + std::to_string(id);
    }
    if (name.size() > 180) {
        std::size_t end = 180;
        while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
        name.resize(end);
    }
    return name;
}

fs::path DownloadManager::uniqueTarget(const std::string &name) const {
    fs::path requested(name);
    fs::path stem = requested.stem();
    fs::path extension = requested.extension();
    fs::path candidate = config_.storageRoot / requested.filename();
    for (std::uint64_t suffix = 1; fs::exists(candidate); ++suffix) {
        candidate = config_.storageRoot /
                    (stem.string() + " (" + std::to_string(suffix) + ")" + extension.string());
    }
    return candidate;
}

json DownloadManager::enqueue(const std::string &rawUrl) {
    const std::string url = validateUrl(rawUrl);
    std::shared_ptr<Task> task;
    std::size_t queuePosition = 0;
    json response;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= 100) throw std::runtime_error("下载队列已满");
        task = std::make_shared<Task>();
        task->id = nextId_++;
        task->url = url;
        task->name = filenameFromUrl(url, task->id);
        task->createdAt = nowMilliseconds();
        tasks_.push_back(task);
        queue_.push_back(task);
        queuePosition = queue_.size();
        response = taskJson(*task, queuePosition);
    }
    wake_.notify_one();
    return response;
}

json DownloadManager::taskJson(const Task &task, std::size_t queuePosition) const {
    std::uint64_t bytes = task.bytes;
    if (!task.temporary.empty()) {
        std::error_code ignored;
        const auto size = fs::file_size(task.temporary, ignored);
        if (!ignored) bytes = size;
    }
    return {{"id", task.id}, {"url", task.url}, {"name", task.name},
            {"status", task.status}, {"error", task.error}, {"bytes", bytes},
            {"queue_position", queuePosition}, {"created_at", task.createdAt},
            {"started_at", task.startedAt}, {"finished_at", task.finishedAt}};
}

json DownloadManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json result = json::array();
    std::size_t position = 0;
    for (const auto &task : tasks_) {
        if (task->status == "queued") ++position;
        result.push_back(taskJson(*task, task->status == "queued" ? position : 0));
    }
    return {{"ok", true}, {"tasks", std::move(result)}};
}

void DownloadManager::work() {
    while (!stopping_.load()) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
            if (stopping_.load()) break;
            task = queue_.front();
            queue_.pop_front();
            task->status = "running";
            task->startedAt = nowMilliseconds();
        }
        download(task);
    }
}

void DownloadManager::download(const std::shared_ptr<Task> &task) {
    fs::path target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = uniqueTarget(task->name);
        task->name = target.filename().string();
        task->temporary = config_.storageRoot /
                          ("." + task->name + ".downloading." + std::to_string(task->id));
    }

    const auto result = runCommand({
        "/usr/bin/curl", "--fail", "--location", "--silent", "--show-error",
        "--proto", "=http,https", "--proto-redir", "=http,https",
        "--connect-timeout", "15", "--speed-time", "30", "--speed-limit", "1024",
        "--max-filesize", "21474836480", "--output", task->temporary.string(), "--", task->url,
    }, std::chrono::hours(24), &stopping_);

    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    if (result.ok() && !stopping_.load()) {
        fs::rename(task->temporary, target, error);
    }
    if (result.ok() && !error && !stopping_.load()) {
        task->status = "completed";
        task->bytes = fs::file_size(target, error);
        task->temporary.clear();
    } else {
        task->status = "failed";
        task->error = stopping_.load() ? "服务正在停止" :
                      (error ? error.message() : trim(result.output));
        if (task->error.empty()) task->error = result.timedOut ? "下载超时" : "下载失败";
        fs::remove(task->temporary, error);
        task->temporary.clear();
    }
    task->finishedAt = nowMilliseconds();
}

}  // namespace remydesk
