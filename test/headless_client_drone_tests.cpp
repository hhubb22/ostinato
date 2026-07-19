#include "client_session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef OSTINATO_DRONE_PATH
#error OSTINATO_DRONE_PATH must name the drone executable
#endif

namespace {

using ostinato::client::ClientSession;

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::uint16_t availablePort()
{
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    require(socketFd >= 0, std::string("socket: ") + std::strerror(errno));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        const std::string error = std::strerror(errno);
        close(socketFd);
        throw std::runtime_error("bind port 0: " + error);
    }
    socklen_t size = sizeof(address);
    if (getsockname(socketFd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        const std::string error = std::strerror(errno);
        close(socketFd);
        throw std::runtime_error("getsockname: " + error);
    }
    const std::uint16_t port = ntohs(address.sin_port);
    close(socketFd);
    return port;
}

class ChildProcess {
public:
    ChildProcess(const char *program, std::uint16_t port)
    {
        char logTemplate[] = "/tmp/ostinato-drone-test-XXXXXX";
        logFd_ = mkstemp(logTemplate);
        require(logFd_ >= 0, std::string("mkstemp: ") + std::strerror(errno));
        unlink(logTemplate);

        pid_ = fork();
        if (pid_ < 0) {
            const std::string error = std::strerror(errno);
            close(logFd_);
            logFd_ = -1;
            throw std::runtime_error("fork: " + error);
        }
        if (pid_ == 0) {
            dup2(logFd_, STDOUT_FILENO);
            dup2(logFd_, STDERR_FILENO);
            close(logFd_);
            const std::string portText = std::to_string(port);
            execl(program, program, "-d", "-p", portText.c_str(),
                  static_cast<char *>(nullptr));
            _exit(127);
        }
    }

    ~ChildProcess()
    {
        stop();
        if (logFd_ >= 0)
            close(logFd_);
    }

    bool running()
    {
        if (pid_ <= 0)
            return false;
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return false;
        }
        return result == 0;
    }

    std::string diagnostics()
    {
        if (logFd_ < 0)
            return {};
        lseek(logFd_, 0, SEEK_SET);
        std::string output;
        char buffer[4096];
        ssize_t count;
        while ((count = read(logFd_, buffer, sizeof(buffer))) > 0)
            output.append(buffer, static_cast<std::size_t>(count));
        return output;
    }

private:
    void stop()
    {
        if (pid_ <= 0)
            return;
        kill(pid_, SIGTERM);
        for (int attempt = 0; attempt < 100; ++attempt) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            usleep(50000);
        }
        kill(pid_, SIGKILL);
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }

    pid_t pid_ = -1;
    int logFd_ = -1;
};

void requireResult(const ClientSession::Result &result, const char *operation)
{
    require(static_cast<bool>(result), std::string(operation) + ": " + result.message);
}

} // namespace

int main()
{
    try {
        const std::uint16_t port = availablePort();
        ChildProcess drone(OSTINATO_DRONE_PATH, port);
        ClientSession client;
        ClientSession::Result connected;
        for (int attempt = 0; attempt < 100; ++attempt) {
            require(drone.running(), "drone exited during startup: " + drone.diagnostics());
            connected = client.connect("127.0.0.1", port, "1.4.99",
                                       std::chrono::milliseconds(200));
            if (connected)
                break;
            usleep(50000);
        }
        require(static_cast<bool>(connected),
                "connect and hydrate: " + connected.message + "\n" + drone.diagnostics());
        require(!client.ports().empty(), "drone exposed no usable ports");

        for (const auto &entry : client.ports()) {
            const auto &state = entry.second;
            require(state.id() == entry.first && state.config().has_port_id() &&
                        state.config().port_id().id() == entry.first,
                    "hydrated port config is structurally incomplete");
            require(state.syncState().syncedStreamIds().size() == state.streams().size(),
                    "hydrated stream snapshot is structurally incomplete");
            require(state.syncState().syncedDeviceGroupIds().size() ==
                        state.deviceGroups().size(),
                    "hydrated device-group snapshot is structurally incomplete");
            (void)state.devices();
        }

        const auto timeout = std::chrono::seconds(3);
        requireResult(client.queryStats(timeout), "initial exact-closure stats query");
        for (const auto &entry : client.ports())
            require(entry.second.stats().has_port_id() &&
                        entry.second.stats().port_id().id() == entry.first,
                    "stats did not close over the hydrated port IDs");

        client.disconnect();
        require(!client.connected() && client.ports().empty(), "explicit disconnect");
        requireResult(client.reconnect(timeout), "reconnect and rehydrate");
        require(!client.ports().empty(), "reconnect exposed no usable ports");
        requireResult(client.queryStats(timeout), "post-reconnect exact-closure stats query");
        client.disconnect();
        require(!client.connected() && client.ports().empty(), "clean final disconnect");

        std::cout << "headless client to drone: PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "headless client to drone: FAIL: " << error.what() << '\n';
        return 1;
    }
}
