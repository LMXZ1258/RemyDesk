#pragma once

#include <atomic>
#include <string>

#include "config.hpp"
#include "file_service.hpp"
#include "httplib.h"
#include "network_manager.hpp"
#include "service_manager.hpp"

namespace remydesk {

class HttpServer {
public:
    explicit HttpServer(Config config);
    bool listen();
    void stop();

private:
    Config config_;
    FileService files_;
    NetworkManager network_;
    ServiceManager services_;
    httplib::Server server_;

    void routes();
    bool authorized(const httplib::Request &request, httplib::Response &response) const;
    static std::string requestHost(const httplib::Request &request);
};

}  // namespace remydesk
