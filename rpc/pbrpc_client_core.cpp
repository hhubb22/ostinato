#include "pbrpc_client_core.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>

namespace pbrpc {

namespace { struct FileCloser { void operator()(std::FILE *file) const { if (file) std::fclose(file); } }; }

struct TcpRpcClient::Connection {
    Connection(int socket, std::uint64_t value) : fd(socket), generation(value) {}
    ~Connection() { ::close(fd); }
    const int fd;
    const std::uint64_t generation;
    std::atomic<int> interrupted{0}; // zero, Canceled, or Disconnected
    std::vector<std::uint8_t> pending;
};

TcpRpcClient::TcpRpcClient(std::uint32_t maxPayload) : maxPayload_(maxPayload) {}
TcpRpcClient::~TcpRpcClient()
{
    interrupt(Error::Disconnected);
    std::lock_guard<std::mutex> barrier(callMutex_);
}

bool TcpRpcClient::connected() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return static_cast<bool>(connection_);
}

void TcpRpcClient::interrupt(Error reason)
{
    std::shared_ptr<Connection> old;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++generation_;
        old.swap(connection_);
        if (old) old->interrupted = static_cast<int>(reason);
    }
    if (old) ::shutdown(old->fd, SHUT_RDWR);
}

void TcpRpcClient::disconnect() { interrupt(Error::Disconnected); }
void TcpRpcClient::cancel() { interrupt(Error::Canceled); }

void TcpRpcClient::setNotificationCallback(NotificationCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    notificationCallback_ = std::move(callback);
}

TcpRpcClient::Result TcpRpcClient::fail(const std::shared_ptr<Connection> &connection,
    Error error, const std::string &message, bool detach)
{
    if (detach) {
        bool shutdown = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (connection_ == connection) {
                ++generation_; connection_.reset(); shutdown = true;
                connection->interrupted = static_cast<int>(error);
            }
        }
        if (shutdown) ::shutdown(connection->fd, SHUT_RDWR);
    }
    return {error, message};
}

TcpRpcClient::Result TcpRpcClient::wait(const std::shared_ptr<Connection> &connection,
                                        short events, Clock::time_point deadline)
{
    for (;;) {
        const int interrupted = connection->interrupted.load();
        if (interrupted) return {static_cast<Error>(interrupted),
            interrupted == static_cast<int>(Error::Canceled) ? "RPC operation canceled" : "RPC connection closed"};
        const Clock::time_point now = Clock::now();
        if (now >= deadline) return fail(connection, Error::Timeout, "RPC operation timed out", true);
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        long long millis = (nanos + 999999) / 1000000; // poll must not expire early
        pollfd event = {connection->fd, events, 0};
        const int rc = ::poll(&event, 1, static_cast<int>(std::min<long long>(millis, INT_MAX)));
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return fail(connection, Error::Transport, std::strerror(errno), true);
        if (!rc) continue; // recompute: poll may return before the absolute deadline
        if (connection->interrupted.load()) continue;
        if (event.revents & events) return {};
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL))
            return fail(connection, Error::Disconnected, "RPC peer disconnected", true);
    }
}

TcpRpcClient::Result TcpRpcClient::connect(const std::string &host, std::uint16_t port,
                                           std::chrono::milliseconds timeout)
{
    std::lock_guard<std::mutex> callLock(callMutex_);
    interrupt(Error::Disconnected);
    const Clock::time_point deadline = Clock::now() + timeout;
    std::uint64_t operation;
    { std::lock_guard<std::mutex> lock(stateMutex_); operation = generation_; }
    addrinfo hints = {}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo *addresses = nullptr;
    const int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
    if (gai) return {Error::Transport, std::string("numeric IPv4/IPv6 address required: ") + gai_strerror(gai)};
    Result result{Error::Transport, "unable to connect"};
    for (addrinfo *it = addresses; it; it = it->ai_next) {
        { std::lock_guard<std::mutex> lock(stateMutex_); if (generation_ != operation) {
            result = {Error::Disconnected, "connect interrupted"}; break; } }
        if (Clock::now() >= deadline) { result = {Error::Timeout, "RPC operation timed out"}; break; }
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) { result.message = std::strerror(errno); continue; }
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            result.message = std::strerror(errno); ::close(fd); continue;
        }
        std::shared_ptr<Connection> candidate(new Connection(fd, operation));
        { std::lock_guard<std::mutex> lock(stateMutex_); if (generation_ != operation) {
            result = {Error::Disconnected, "connect interrupted"}; break; } connection_ = candidate; }
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) result = {};
        else if (errno == EINPROGRESS) {
            result = wait(candidate, POLLOUT, deadline);
            if (result) {
                int error = 0; socklen_t length = sizeof(error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
                    result = fail(candidate, Error::Transport, std::strerror(error ? error : errno), true);
            }
        } else result = fail(candidate, Error::Transport, std::strerror(errno), true);
        if (result) break;
        if (result.error == Error::Canceled || result.error == Error::Disconnected || result.error == Error::Timeout) break;
        fail(candidate, result.error, result.message, true);
    }
    freeaddrinfo(addresses);
    return result;
}

TcpRpcClient::Result TcpRpcClient::writeAll(const std::shared_ptr<Connection> &connection,
    const std::uint8_t *data, std::size_t size, Clock::time_point deadline)
{
    while (size) {
        Result ready = wait(connection, POLLOUT, deadline); if (!ready) return ready;
        const ssize_t count = ::send(connection->fd, data, size, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return fail(connection, Error::Transport,
            count ? std::strerror(errno) : "short socket write", true);
        data += count; size -= static_cast<std::size_t>(count);
    }
    return {};
}

TcpRpcClient::Result TcpRpcClient::readPayload(const std::shared_ptr<Connection> &connection,
    std::uint32_t size, std::vector<std::uint8_t> &payload, Clock::time_point deadline,
    std::FILE *spool)
{
    std::uint32_t remaining = size;
    while (remaining) {
        if (connection->pending.empty()) {
            Result ready = wait(connection, POLLIN, deadline); if (!ready) return ready;
            std::uint8_t input[64 * 1024];
            const std::size_t wanted = std::min<std::size_t>(remaining, sizeof(input));
            const ssize_t count = ::recv(connection->fd, input, wanted, 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (count <= 0) return fail(connection, count ? Error::Transport : Error::Disconnected,
                count ? std::strerror(errno) : "RPC peer disconnected", true);
            connection->pending.assign(input, input + count);
        }
        const std::size_t count = std::min<std::size_t>(remaining, connection->pending.size());
        if (spool) {
            if (std::fwrite(connection->pending.data(), 1, count, spool) != count)
                return fail(connection, Error::Transport, "could not spool RPC binary blob", true);
        }
        else payload.insert(payload.end(), connection->pending.begin(), connection->pending.begin() + count);
        connection->pending.erase(connection->pending.begin(), connection->pending.begin() + count);
        remaining -= static_cast<std::uint32_t>(count);
    }
    return {};
}

TcpRpcClient::Result TcpRpcClient::nextHeader(const std::shared_ptr<Connection> &connection,
                                               Header &header, Clock::time_point deadline)
{
    while (connection->pending.size() < kHeaderSize) {
        Result ready = wait(connection, POLLIN, deadline); if (!ready) return ready;
        std::uint8_t input[8192];
        const ssize_t count = ::recv(connection->fd, input, sizeof(input), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return fail(connection, count ? Error::Transport : Error::Disconnected,
            count ? std::strerror(errno) : "RPC peer disconnected", true);
        connection->pending.insert(connection->pending.end(), input, input + count);
    }
    decodeHeader(ByteView(connection->pending.data(), kHeaderSize), header);
    connection->pending.erase(connection->pending.begin(), connection->pending.begin() + kHeaderSize);
    if (!isValidMessageType(static_cast<std::uint16_t>(header.type)))
        return fail(connection, Error::Protocol, "invalid RPC message type", true);
    if (header.length > maxPayload_)
        return fail(connection, Error::Protocol, "RPC payload exceeds configured maximum", true);
    return {};
}

TcpRpcClient::Result TcpRpcClient::call(const google::protobuf::MethodDescriptor *method,
    const google::protobuf::Message &request, google::protobuf::Message *response,
    std::chrono::milliseconds timeout, std::vector<std::uint8_t> *blob, BlobSink blobSink)
{
    struct Notice { std::uint16_t method; std::vector<std::uint8_t> payload; };
    std::vector<Notice> notices;
    std::unique_ptr<std::FILE, FileCloser> blobSpool(blobSink ? std::tmpfile() : nullptr);
    if (blobSink && !blobSpool) return {Error::Transport, "could not create binary blob spool"};
    Result result;
    {
        std::lock_guard<std::mutex> lock(callMutex_);
        std::shared_ptr<Connection> connection;
        { std::lock_guard<std::mutex> stateLock(stateMutex_); connection = connection_; }
        if (!connection) return {Error::Disconnected, "RPC client is not connected"};
        if (!method || method->index() < 0 || method->index() > std::numeric_limits<std::uint16_t>::max()
                || request.GetDescriptor() != method->input_type()
                || (response && response->GetDescriptor() != method->output_type()))
            return {Error::InvalidProtobuf, "protobuf method/request/response type mismatch"};
        if (!request.IsInitialized()) return {Error::InvalidProtobuf, "RPC request missing required fields"};
        std::string payload; if (!request.SerializeToString(&payload))
            return {Error::InvalidProtobuf, "could not serialize RPC request"};
        std::vector<std::uint8_t> wire;
        try { wire = encodeFrame(MessageType::Request, static_cast<std::uint16_t>(method->index()), ByteView(payload.data(), payload.size())); }
        catch (const std::exception &error) { return {Error::InvalidProtobuf, error.what()}; }
        const Clock::time_point deadline = Clock::now() + timeout;
        result = writeAll(connection, wire.data(), wire.size(), deadline);
        while (result) {
            Header header; result = nextHeader(connection, header, deadline); if (!result) break;
            std::vector<std::uint8_t> body;
            if (header.type == MessageType::BinaryBlob) {
                if (header.method != method->index()) { result = fail(connection, Error::Protocol, "RPC response method id mismatch", true); break; }
                result = readPayload(connection, header.length, body, deadline, blobSpool.get());
                if (result && blob && !blobSink) blob->insert(blob->end(), body.begin(), body.end());
                break; // BINBLOB is unconditionally terminal; pending trailing bytes remain queued.
            }
            result = readPayload(connection, header.length, body, deadline); if (!result) break;
            if (header.type == MessageType::Notification) { notices.push_back({header.method, std::move(body)}); continue; }
            if (header.method != method->index()) { result = fail(connection, Error::Protocol, "RPC response method id mismatch", true); break; }
            if (header.type == MessageType::Error) { result = {Error::Remote, std::string(body.begin(), body.end())}; break; }
            if (header.type != MessageType::Response || !response) { result = fail(connection, Error::Protocol, "unexpected RPC response type", true); break; }
            response->Clear();
            if (!response->ParseFromArray(body.data(), static_cast<int>(body.size())) || !response->IsInitialized())
                result = fail(connection, Error::InvalidProtobuf, "invalid or incomplete protobuf response", true);
            break;
        }
    }
    NotificationCallback callback;
    { std::lock_guard<std::mutex> lock(callbackMutex_); callback = notificationCallback_; }
    if (callback) for (const auto &notice : notices) { try { callback(notice.method, notice.payload); } catch (...) {} }
    if (result && blobSink) {
        std::rewind(blobSpool.get());
        std::uint8_t chunk[64 * 1024];
        for (;;) {
            const std::size_t count = std::fread(chunk, 1, sizeof(chunk), blobSpool.get());
            if (!count) break;
            if (blob) blob->insert(blob->end(), chunk, chunk + count);
            bool accepted = false; try { accepted = blobSink(chunk, count); } catch (...) {}
            if (!accepted) { interrupt(Error::Protocol); return {Error::Protocol, "binary blob sink rejected data"}; }
        }
    }
    return result;
}

} // namespace pbrpc
