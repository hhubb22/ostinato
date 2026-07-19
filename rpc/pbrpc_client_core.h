/* Qt-free POSIX client for the Ostinato protobuf RPC protocol. */
#ifndef PBRPC_CLIENT_CORE_H
#define PBRPC_CLIENT_CORE_H

#include "pbrpc_core.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pbrpc {

class TcpRpcClient {
public:
    enum class Error { None, Timeout, Canceled, Disconnected, Transport, Remote,
                       Protocol, InvalidProtobuf };
    struct Result {
        Error error = Error::None;
        std::string message;
        Result() = default;
        Result(Error value, std::string text) : error(value), message(std::move(text)) {}
        explicit operator bool() const { return error == Error::None; }
    };
    using BlobSink = std::function<bool(const std::uint8_t *, std::size_t)>;
    using NotificationCallback = std::function<void(std::uint16_t,
        const std::vector<std::uint8_t> &)>;

    explicit TcpRpcClient(std::uint32_t maxPayload = kDefaultMaxPayload);
    ~TcpRpcClient();
    TcpRpcClient(const TcpRpcClient &) = delete;
    TcpRpcClient &operator=(const TcpRpcClient &) = delete;

    // Hostname lookup is deliberately excluded: numeric IPv4/IPv6 makes the
    // supplied deadline and cancellation apply to every connect stage.
    Result connect(const std::string &numericHost, std::uint16_t port,
                   std::chrono::milliseconds timeout);
    void disconnect();
    bool connected() const;
    Result call(const google::protobuf::MethodDescriptor *method,
                const google::protobuf::Message &request,
                google::protobuf::Message *response,
                std::chrono::milliseconds timeout,
                std::vector<std::uint8_t> *blob = nullptr,
                BlobSink blobSink = BlobSink());
    void cancel();
    void setNotificationCallback(NotificationCallback callback);

private:
    struct Connection;
    typedef std::chrono::steady_clock Clock;
    Result wait(const std::shared_ptr<Connection> &, short, Clock::time_point);
    Result writeAll(const std::shared_ptr<Connection> &, const std::uint8_t *,
                    std::size_t, Clock::time_point);
    Result nextHeader(const std::shared_ptr<Connection> &, Header &,
                      Clock::time_point);
    Result readPayload(const std::shared_ptr<Connection> &, std::uint32_t,
                       std::vector<std::uint8_t> &, Clock::time_point,
                       std::FILE *spool = nullptr);
    void interrupt(Error reason);
    Result fail(const std::shared_ptr<Connection> &, Error, const std::string &,
                bool detach);

    const std::uint32_t maxPayload_;
    mutable std::mutex stateMutex_;
    std::shared_ptr<Connection> connection_;
    std::uint64_t generation_ = 0;
    std::mutex callMutex_;
    std::mutex callbackMutex_;
    NotificationCallback notificationCallback_;
};

} // namespace pbrpc
#endif
