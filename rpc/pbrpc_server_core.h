/* Qt-free POSIX TCP server for the Ostinato protobuf RPC protocol. */
#ifndef PBRPC_SERVER_CORE_H
#define PBRPC_SERVER_CORE_H

#include "pbrpc_core.h"

#include <google/protobuf/service.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbrpc {

class ServerController : public google::protobuf::RpcController {
public:
    void Reset() override;
    bool Failed() const override;
    std::string ErrorText() const override;
    void StartCancel() override;
    void SetFailed(const std::string &reason) override;
    bool IsCanceled() const override;
    void NotifyOnCancel(google::protobuf::Closure *callback) override;

    void triggerDisconnect() { disconnect_ = true; }
    bool disconnect() const { return disconnect_; }
    void enableNotifications(bool enabled) { notifications_ = enabled; }
    bool notificationsEnabled() const { return notifications_; }
    void setBinaryBlob(std::vector<std::uint8_t> blob) { hasBlob_ = true; blob_ = std::move(blob); }
    bool hasBinaryBlob() const { return hasBlob_; }
    const std::vector<std::uint8_t> &binaryBlob() const { return blob_; }

private:
    bool failed_ = false;
    bool disconnect_ = false;
    bool notifications_ = true;
    bool hasBlob_ = false;
    std::string error_;
    std::vector<std::uint8_t> blob_;
};

class TcpRpcServer {
public:
    struct Options {
        std::string address = "0.0.0.0";
        std::uint16_t port = 0;
        std::uint32_t maxPayload = kDefaultMaxPayload;
    };

    explicit TcpRpcServer(google::protobuf::Service *service);
    TcpRpcServer(google::protobuf::Service *service, Options options);
    ~TcpRpcServer();
    TcpRpcServer(const TcpRpcServer &) = delete;
    TcpRpcServer &operator=(const TcpRpcServer &) = delete;

    bool start(std::string *error = nullptr);
    void stop();
    std::uint16_t port() const { return boundPort_; }
    bool running() const { return running_; }

    // Sent only to connections which completed method 15 compatibility and
    // whose method-15 controller left notifications enabled.
    void broadcastNotification(std::uint16_t type,
                               const google::protobuf::Message &message);

private:
    class Connection;
    void acceptLoop();
    void removeClosedConnections();

    google::protobuf::Service *service_; // not owned
    Options options_;
    int listenFd_ = -1;
    std::uint16_t boundPort_ = 0;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::mutex connectionsMutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
};

} // namespace pbrpc
#endif
