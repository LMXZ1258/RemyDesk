#include "process.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <stdexcept>

namespace remydesk {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

CommandResult runCommand(const std::vector<std::string> &arguments,
                         std::chrono::milliseconds timeout,
                         const std::atomic_bool *cancel) {
    if (arguments.empty()) throw std::invalid_argument("empty command");

    int pipeFd[2];
    if (pipe2(pipeFd, O_CLOEXEC | O_NONBLOCK) != 0) {
        throw std::runtime_error("pipe2 failed");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipeFd[0]);
        close(pipeFd[1]);
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        dup2(pipeFd[1], STDOUT_FILENO);
        dup2(pipeFd[1], STDERR_FILENO);
        close(pipeFd[0]);
        close(pipeFd[1]);

        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto &argument : arguments) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipeFd[1]);
    CommandResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool pipeOpen = true;
    bool childDone = false;
    int status = 0;

    while (pipeOpen || !childDone) {
        if (!childDone) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            childDone = waited == pid;
        }

        if (!childDone && (std::chrono::steady_clock::now() >= deadline ||
                           (cancel && cancel->load()))) {
            result.timedOut = std::chrono::steady_clock::now() >= deadline;
            kill(pid, SIGTERM);
            usleep(100000);
            if (waitpid(pid, &status, WNOHANG) == 0) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            childDone = true;
        }

        if (pipeOpen) {
            pollfd descriptor{pipeFd[0], POLLIN | POLLHUP, 0};
            poll(&descriptor, 1, 50);
            std::array<char, 4096> buffer{};
            while (true) {
                const ssize_t count = read(pipeFd[0], buffer.data(), buffer.size());
                if (count > 0) {
                    result.output.append(buffer.data(), static_cast<std::size_t>(count));
                    continue;
                }
                if (count == 0) pipeOpen = false;
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) pipeOpen = false;
                break;
            }
        }
    }

    close(pipeFd[0]);
    if (!result.timedOut) {
        if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    }
    return result;
}

}  // namespace remydesk
