#include "http_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "hardware_probe.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace remydesk {

namespace {

json bodyJson(const httplib::Request &request) {
    if (request.body.empty()) return json::object();
    try {
        return json::parse(request.body);
    } catch (const json::exception &) {
        throw std::invalid_argument("请求正文不是有效 JSON");
    }
}

void sendJson(httplib::Response &response, const json &value, int status = 200) {
    response.status = status;
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

void sendError(httplib::Response &response, const std::exception &exception, int status = 400) {
    sendJson(response, {{"ok", false}, {"error", exception.what()}}, status);
}

std::string mimeType(const fs::path &path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js") return "application/javascript; charset=utf-8";
    if (extension == ".json") return "application/json; charset=utf-8";
    if (extension == ".txt" || extension == ".log") return "text/plain; charset=utf-8";
    if (extension == ".md" || extension == ".markdown") return "text/markdown; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".bmp") return "image/bmp";
    if (extension == ".mp4" || extension == ".m4v") return "video/mp4";
    if (extension == ".webm") return "video/webm";
    if (extension == ".mov") return "video/quicktime";
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".wav") return "audio/wav";
    if (extension == ".ogg") return "audio/ogg";
    if (extension == ".pdf") return "application/pdf";
    if (extension == ".epub") return "application/epub+zip";
    return "application/octet-stream";
}

std::string readFile(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("无法读取文件");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

HttpServer::HttpServer(Config config)
    : config_(std::move(config)), files_(config_), network_(config_), services_(config_) {
    server_.set_payload_max_length(1024ULL * 1024ULL * 1024ULL);
    server_.set_read_timeout(60, 0);
    server_.set_write_timeout(60, 0);
    routes();
}

bool HttpServer::authorized(const httplib::Request &request, httplib::Response &response) const {
    if (config_.authPassword.empty()) return true;
    const auto auth = request.get_header_value("Authorization");
    if (auth.empty() || !request.has_header("Authorization")) {
        response.status = 401;
        response.set_header("WWW-Authenticate", "Basic realm=\"RemyDesk\"");
        return false;
    }
    const auto expected = httplib::make_basic_authentication_header(config_.authUser, config_.authPassword);
    if (auth != expected.second) {
        response.status = 401;
        response.set_header("WWW-Authenticate", "Basic realm=\"RemyDesk\"");
        return false;
    }
    return true;
}

std::string HttpServer::requestHost(const httplib::Request &request) {
    std::string host = request.get_header_value("Host");
    if (host.empty()) return "127.0.0.1";
    if (host.front() == '[') {
        const auto end = host.find(']');
        return end == std::string::npos ? host : host.substr(0, end + 1);
    }
    const auto colon = host.find(':');
    return colon == std::string::npos ? host : host.substr(0, colon);
}

void HttpServer::routes() {
    server_.set_pre_routing_handler([this](const auto &request, auto &response) {
        return authorized(request, response) ? httplib::Server::HandlerResponse::Unhandled
                                             : httplib::Server::HandlerResponse::Handled;
    });

    server_.Get("/", [this](const auto &, auto &response) {
        try { response.set_content(readFile(config_.webRoot / "index.html"), "text/html; charset=utf-8"); }
        catch (const std::exception &exception) { sendError(response, exception, 500); }
    });
    server_.Get(R"(/assets/(.+))", [this](const auto &request, auto &response) {
        try {
            const fs::path relative = request.matches[1].str();
            if (relative.empty() || relative.is_absolute() || relative.string().find("..") != std::string::npos) {
                throw std::invalid_argument("非法资源路径");
            }
            const fs::path path = config_.webRoot / relative;
            if (!fs::is_regular_file(path)) throw std::invalid_argument("资源不存在");
            response.set_file_content(path.string(), mimeType(path));
        } catch (const std::exception &exception) { sendError(response, exception, 404); }
    });

    server_.Get("/api/health", [this](const auto &, auto &response) {
        sendJson(response, {{"ok", true}, {"service", "remydeskd"}, {"version", "0.1.0"},
                            {"storage_root", config_.storageRoot.string()}});
    });
    server_.Get("/api/hardware", [this](const auto &, auto &response) {
        try { sendJson(response, probeHardware()); }
        catch (const std::exception &exception) { sendError(response, exception, 500); }
    });
    server_.Get("/api/files", [this](const auto &request, auto &response) {
        try { sendJson(response, files_.list(request.get_param_value("path"))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Get("/file", [this](const auto &request, auto &response) {
        try {
            const fs::path path = files_.resolve(request.get_param_value("path"));
            if (!fs::is_regular_file(path)) throw std::invalid_argument("路径不是文件");
            response.set_header("Content-Disposition", "attachment; filename=\"" + path.filename().string() + "\"");
            response.set_file_content(path.string(), mimeType(path));
        } catch (const std::exception &exception) { sendError(response, exception, 404); }
    });
    server_.Get("/preview", [this](const auto &request, auto &response) {
        try {
            const fs::path path = files_.resolve(request.get_param_value("path"));
            if (!fs::is_regular_file(path)) throw std::invalid_argument("路径不是文件");
            response.set_header("Content-Disposition", "inline; filename=\"" + path.filename().string() + "\"");
            response.set_header("X-Content-Type-Options", "nosniff");
            response.set_header("Accept-Ranges", "bytes");
            const auto size = static_cast<std::size_t>(fs::file_size(path));
            response.set_content_provider(
                size, mimeType(path),
                [path](std::size_t offset, std::size_t length, httplib::DataSink &sink) {
                    std::ifstream stream(path, std::ios::binary);
                    if (!stream) return false;
                    stream.seekg(static_cast<std::streamoff>(offset));
                    std::vector<char> buffer(length);
                    stream.read(buffer.data(), static_cast<std::streamsize>(length));
                    const auto count = static_cast<std::size_t>(stream.gcount());
                    if (count) sink.write(buffer.data(), count);
                    return count == length;
                });
        } catch (const std::exception &exception) { sendError(response, exception, 404); }
    });
    server_.Post("/api/upload", [this](const auto &request, auto &response,
                                       const httplib::ContentReader &contentReader) {
        fs::path temporary;
        try {
            const std::string name = request.get_param_value("name");
            if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos) {
                throw std::invalid_argument("非法文件名");
            }
            const fs::path directory = files_.resolve(request.get_param_value("path"));
            const fs::path target = directory / name;
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            temporary = directory / ("." + name + ".uploading." + std::to_string(nonce));
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("无法创建上传文件");
            const bool received = contentReader([&stream](const char *data, std::size_t length) {
                stream.write(data, static_cast<std::streamsize>(length));
                return stream.good();
            });
            stream.close();
            if (!received || !stream) throw std::runtime_error("上传中断");
            fs::rename(temporary, target);
            sendJson(response, {{"ok", true}, {"path", fs::relative(target, config_.storageRoot).generic_string()}});
        } catch (const std::exception &exception) {
            if (!temporary.empty()) { std::error_code ignored; fs::remove(temporary, ignored); }
            sendError(response, exception);
        }
    });
    server_.Post("/api/mkdir", [this](const auto &request, auto &response) {
        try { const auto body = bodyJson(request); sendJson(response, files_.createDirectory(body.value("path", ""), body.value("name", ""))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Post("/api/delete", [this](const auto &request, auto &response) {
        try { sendJson(response, files_.remove(bodyJson(request).value("path", ""))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Post("/api/move", [this](const auto &request, auto &response) {
        try {
            const auto body = bodyJson(request);
            sendJson(response, files_.move(body.value("source", ""), body.value("destination", "")));
        } catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Get("/api/desktop/layout", [this](const auto &, auto &response) {
        try { sendJson(response, files_.readDesktopLayout()); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Post("/api/desktop/layout", [this](const auto &request, auto &response) {
        try { sendJson(response, files_.writeDesktopLayout(bodyJson(request).value("layout", json::object()))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Get("/api/note", [this](const auto &, auto &response) {
        try { sendJson(response, files_.readNote()); } catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Post("/api/note", [this](const auto &request, auto &response) {
        try { sendJson(response, files_.writeNote(bodyJson(request).value("text", ""))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });

    server_.Get("/api/desktop/status", [this](const auto &request, auto &response) {
        try { sendJson(response, services_.desktopStatus(requestHost(request))); }
        catch (const std::exception &exception) { sendError(response, exception, 500); }
    });
    server_.Post("/api/desktop/switch", [this](const auto &request, auto &response) {
        try { sendJson(response, services_.switchDesktop(bodyJson(request).value("enabled", false), requestHost(request))); }
        catch (const std::exception &exception) { sendError(response, exception, 500); }
    });

    server_.Get("/api/network/status", [this](const auto &, auto &response) {
        try { sendJson(response, network_.status()); } catch (const std::exception &exception) { sendError(response, exception, 500); }
    });
    server_.Get("/api/network/scan", [this](const auto &request, auto &response) {
        try { sendJson(response, network_.scan(request.get_param_value("deep") == "1")); }
        catch (const std::exception &exception) { sendError(response, exception, 500); }
    });
    server_.Post("/api/network/connect", [this](const auto &request, auto &response) {
        try { const auto body = bodyJson(request); sendJson(response, network_.connectWifi(body.value("ssid", ""), body.value("password", ""), body.value("known", false))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Post("/api/network/hotspot/start", [this](const auto &, auto &response) {
        try { sendJson(response, network_.startHotspot()); } catch (const std::exception &exception) { sendError(response, exception, 500); }
    });
    server_.Post("/api/network/hotspot/stop", [this](const auto &, auto &response) {
        try { sendJson(response, network_.stopHotspot()); } catch (const std::exception &exception) { sendError(response, exception, 500); }
    });
    server_.Post("/api/network/settings", [this](const auto &request, auto &response) {
        try { sendJson(response, network_.setAutoHotspot(bodyJson(request).value("auto_hotspot", true))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Get("/api/network/ipv4", [this](const auto &request, auto &response) {
        try { sendJson(response, network_.ipv4(request.get_param_value("connection"))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Post("/api/network/ipv4", [this](const auto &request, auto &response) {
        try { sendJson(response, network_.setIpv4(bodyJson(request))); }
        catch (const std::exception &exception) { sendError(response, exception); }
    });
    server_.Post("/api/system/shutdown", [this](const auto &, auto &response) {
        try { sendJson(response, services_.poweroff()); } catch (const std::exception &exception) { sendError(response, exception, 500); }
    });

    server_.set_error_handler([](const auto &, auto &response) {
        if (response.body.empty()) sendJson(response, {{"ok", false}, {"error", "Not Found"}}, response.status);
    });
}

bool HttpServer::listen() {
    std::cout << "RemyDesk listening on http://" << config_.host << ':' << config_.port << '\n';
    network_.maybeStartHotspot();
    return server_.listen(config_.host, config_.port);
}

void HttpServer::stop() { server_.stop(); }

}  // namespace remydesk
