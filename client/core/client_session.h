#ifndef OSTINATO_CLIENT_CORE_CLIENT_SESSION_H
#define OSTINATO_CLIENT_CORE_CLIENT_SESSION_H

#include "port_state.h"
#include "pbrpc_client_core.h"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace ostinato { namespace client {

class ClientSession {
public:
    using Result = pbrpc::TcpRpcClient::Result;
    using BlobSink = pbrpc::TcpRpcClient::BlobSink;
    ClientSession();
    ~ClientSession();
    Result connect(const std::string &numericHost, std::uint16_t port,
                   const std::string &version, std::chrono::milliseconds timeout);
    Result reconnect(std::chrono::milliseconds timeout);
    void disconnect();
    void cancel();
    bool connected() const;
    // State access is owner-thread-only. Returned references/pointers remain
    // valid only until the next application operation.
    const std::map<std::uint32_t, PortState> &ports() const { return ports_; }
    PortState *port(std::uint32_t id);
    Result apply(std::uint32_t portId, std::chrono::milliseconds timeout);
    Result startTransmit(std::uint32_t portId, std::chrono::milliseconds timeout);
    Result stopTransmit(std::uint32_t portId, std::chrono::milliseconds timeout);
    Result startCapture(std::uint32_t portId, std::chrono::milliseconds timeout);
    Result stopCapture(std::uint32_t portId, std::chrono::milliseconds timeout);
    Result queryStats(std::chrono::milliseconds timeout);
    Result pollStats(std::chrono::milliseconds timeout) { return queryStats(timeout); }
    Result getCapture(std::uint32_t portId, std::vector<std::uint8_t> &data,
                      std::chrono::milliseconds timeout, BlobSink sink = BlobSink());
private:
    Result connectUnlocked(const std::string &numericHost, std::uint16_t port,
                           const std::string &version, std::chrono::milliseconds timeout);
    void clearIfDisconnected();
    Result call(const char *name, const google::protobuf::Message &request,
                google::protobuf::Message &response, std::chrono::milliseconds timeout,
                std::vector<std::uint8_t> *blob = nullptr, BlobSink sink = BlobSink());
    Result hydrate(std::chrono::milliseconds timeout);
    Result hydratePort(std::uint32_t id, const OstProto::Port &config,
                       std::chrono::milliseconds timeout);
    Result ackCall(const char *name, const google::protobuf::Message &request,
                   std::chrono::milliseconds timeout);
    Result processNotifications(std::chrono::milliseconds timeout);
    Result control(const char *name, std::uint32_t id, std::chrono::milliseconds timeout);
    pbrpc::TcpRpcClient rpc_;
    mutable std::recursive_mutex operationMutex_;
    std::mutex notificationMutex_;
    std::map<std::uint32_t, PortState> ports_;
    std::set<std::uint32_t> pendingNotifications_;
    std::string host_, version_;
    std::uint16_t endpointPort_ = 0;
    bool processingNotifications_ = false;
};
} }
#endif
