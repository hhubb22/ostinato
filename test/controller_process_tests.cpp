#include "stdio_protocol.h"

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef OSTINATO_CONTROLLER_PATH
#error OSTINATO_CONTROLLER_PATH must name the controller executable
#endif

using namespace ostinato::controller;

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class Child {
public:
    Child()
    {
        int input[2], output[2];
        require(pipe(input) == 0 && pipe(output) == 0,
                std::string("pipe: ") + std::strerror(errno));
        pid_ = fork();
        require(pid_ >= 0, std::string("fork: ") + std::strerror(errno));
        if (pid_ == 0) {
            dup2(input[0], STDIN_FILENO);
            dup2(output[1], STDOUT_FILENO);
            close(input[0]); close(input[1]); close(output[0]); close(output[1]);
            execl(OSTINATO_CONTROLLER_PATH, OSTINATO_CONTROLLER_PATH,
                  static_cast<char *>(nullptr));
            _exit(127);
        }
        close(input[0]); close(output[1]);
        input_ = input[1];
        output_ = output[0];
    }

    ~Child()
    {
        if (input_ >= 0) close(input_);
        if (output_ >= 0) close(output_);
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

    void writeJson(const std::string &json)
    {
        const auto bytes = encodeFrame(FrameType::Json, json);
        // Deliberately split the first frame to exercise process buffering.
        const std::size_t split = bytes.size() / 2;
        writeAll(bytes.data(), split);
        writeAll(bytes.data() + split, bytes.size() - split);
    }

    std::string readJson()
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<Frame> frames;
            std::string error;
            if (!decoder_.feed(nullptr, 0, frames, error))
                throw std::runtime_error(error);
            if (!pending_.empty()) {
                const std::string result = pending_.front();
                pending_.erase(pending_.begin());
                return result;
            }
            pollfd fd = {output_, POLLIN, 0};
            if (poll(&fd, 1, 100) <= 0)
                continue;
            std::uint8_t bytes[4096];
            const ssize_t count = read(output_, bytes, sizeof(bytes));
            require(count > 0, "controller stdout closed before response");
            frames.clear();
            require(decoder_.feed(bytes, static_cast<std::size_t>(count), frames, error),
                    "controller stdout is not clean framed data: " + error);
            for (const auto &frame : frames) {
                require(frame.type == static_cast<std::uint8_t>(FrameType::Json),
                        "unexpected stdout frame type");
                pending_.emplace_back(frame.payload.begin(), frame.payload.end());
            }
            if (!pending_.empty()) {
                const std::string result = pending_.front();
                pending_.erase(pending_.begin());
                return result;
            }
        }
        throw std::runtime_error("timed out waiting for controller frame");
    }

    void expectExit()
    {
        close(input_); input_ = -1;
        for (int attempt = 0; attempt < 100; ++attempt) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                        "controller shutdown status");
                return;
            }
            usleep(20000);
        }
        throw std::runtime_error("controller did not terminate and reap");
    }

private:
    void writeAll(const std::uint8_t *data, std::size_t size)
    {
        while (size) {
            const ssize_t count = write(input_, data, size);
            require(count > 0, std::string("write: ") + std::strerror(errno));
            data += count;
            size -= static_cast<std::size_t>(count);
        }
    }

    pid_t pid_ = -1;
    int input_ = -1, output_ = -1;
    FrameDecoder decoder_;
    std::vector<std::string> pending_;
};

bool contains(const std::string &value, const std::string &part)
{
    return value.find(part) != std::string::npos;
}

} // namespace

int main()
{
    try {
        Child child;
        require(contains(child.readJson(), "\"event\":\"status\"") ||
                    contains(child.readJson(), "\"event\": \"status\""),
                "startup status event");

        child.writeJson("{not-json");
        require(contains(child.readJson(), "protocolError"),
                "malformed JSON returns a controlled event");

        child.writeJson("{\"kind\":\"request\",\"id\":\"req-7\","
                        "\"command\":\"doesNotExist\",\"args\":{}}");
        const std::string unknown = child.readJson();
        require(contains(unknown, "\"id\":\"req-7\"") &&
                    contains(unknown, "unknown-command") &&
                    contains(unknown, "\"ok\":false"),
                "unknown command preserves request/response ID");

        child.writeJson("{\"kind\":\"request\",\"id\":\"stop-1\","
                        "\"command\":\"shutdown\",\"args\":{}}");
        const std::string shutdown = child.readJson();
        require(contains(shutdown, "\"id\":\"stop-1\"") &&
                    contains(shutdown, "\"ok\":true"),
                "shutdown response ID");
        child.expectExit();
        std::cout << "controller process protocol tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "controller process protocol tests failed: " << error.what() << '\n';
        return 1;
    }
}
