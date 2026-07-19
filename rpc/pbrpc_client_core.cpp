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

namespace {
struct FileCloser {
    void operator()(std::FILE *file) const { if (file) std::fclose(file); }
};

#ifdef PBRPC_CLIENT_CORE_TESTING
std::mutex connectTestHookMutex;
ClientConnectTestHook connectTestHook;
ClientDestructorTestHook destructorTestHook;

void runConnectTestHook(ClientConnectTestPhase phase)
{
    ClientConnectTestHook hook;
    {
        std::lock_guard<std::mutex> lock(connectTestHookMutex);
        hook = connectTestHook;
    }
    if (hook)
        hook(phase);
}

void runDestructorTestHook()
{
    ClientDestructorTestHook hook;
    {
        std::lock_guard<std::mutex> lock(connectTestHookMutex);
        hook = destructorTestHook;
    }
    if (hook)
        hook();
}
#endif
} // namespace

#ifdef PBRPC_CLIENT_CORE_TESTING
void setClientConnectTestHook(ClientConnectTestHook hook)
{
    std::lock_guard<std::mutex> lock(connectTestHookMutex);
    connectTestHook = std::move(hook);
}

void setClientDestructorTestHook(ClientDestructorTestHook hook)
{
    std::lock_guard<std::mutex> lock(connectTestHookMutex);
    destructorTestHook = std::move(hook);
}
#endif

struct TcpRpcClient::Connection {
    Connection(int socket, std::uint64_t value) : fd(socket), generation(value) {}
    ~Connection() { ::close(fd); }
    enum State { Connecting, Connected };
    const int fd;
    const std::uint64_t generation;
    std::atomic<int> state{Connecting};
    std::atomic<int> interrupted{0}; // zero, Canceled, or Disconnected
    std::vector<std::uint8_t> pending;
};

struct TcpRpcClient::ConnectAttempt {
    std::atomic<int> interrupted{0}; // first Canceled or Disconnected reason wins
};

class TcpRpcClient::Invocation {
public:
    explicit Invocation(TcpRpcClient *client)
        : client_(client), active_(client_->beginInvocation()) {}
    ~Invocation()
    {
        if (active_)
            client_->endInvocation();
    }
    explicit operator bool() const { return active_; }

private:
    TcpRpcClient *client_;
    bool active_;
};

class TcpRpcClient::ConnectInvocation {
public:
    explicit ConnectInvocation(TcpRpcClient *client) : client_(client)
    {
        active_ = client_->beginConnectInvocation(attempt_);
    }
    ~ConnectInvocation()
    {
        if (active_)
            client_->endConnectInvocation(attempt_);
    }
    explicit operator bool() const { return active_; }
    const std::shared_ptr<ConnectAttempt> &attempt() const { return attempt_; }

private:
    TcpRpcClient *client_;
    std::shared_ptr<ConnectAttempt> attempt_;
    bool active_ = false;
};

TcpRpcClient::TcpRpcClient(std::uint32_t maxPayload) : maxPayload_(maxPayload) {}
TcpRpcClient::~TcpRpcClient()
{
    {
        std::lock_guard<std::mutex> lock(invocationMutex_);
        shuttingDown_ = true;
    }
    interrupt(Error::Disconnected);
#ifdef PBRPC_CLIENT_CORE_TESTING
    runDestructorTestHook();
#endif
    std::unique_lock<std::mutex> lock(invocationMutex_);
    invocationCv_.wait(lock, [this] { return activeInvocations_ == 0; });
}

bool TcpRpcClient::beginInvocation()
{
    std::lock_guard<std::mutex> lock(invocationMutex_);
    if (shuttingDown_)
        return false;
    ++activeInvocations_;
    return true;
}

void TcpRpcClient::endInvocation()
{
    std::lock_guard<std::mutex> lock(invocationMutex_);
    if (--activeInvocations_ == 0)
        invocationCv_.notify_all();
}

bool TcpRpcClient::connected() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connection_ &&
        connection_->state.load() == Connection::Connected &&
        connection_->interrupted.load() == 0;
}

void TcpRpcClient::interrupt(Error reason)
{
    std::shared_ptr<Connection> old;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto &attempt : connectAttempts_) {
            int expected = 0;
            attempt->interrupted.compare_exchange_strong(
                expected, static_cast<int>(reason));
        }
        ++generation_;
        old.swap(connection_);
        if (old) {
            int expected = 0;
            old->interrupted.compare_exchange_strong(
                expected, static_cast<int>(reason));
        }
    }
    if (old) ::shutdown(old->fd, SHUT_RDWR);
}

void TcpRpcClient::disconnect() { interrupt(Error::Disconnected); }
void TcpRpcClient::cancel() { interrupt(Error::Canceled); }

bool TcpRpcClient::beginConnectInvocation(
    std::shared_ptr<ConnectAttempt> &attempt)
{
    attempt.reset(new ConnectAttempt);
    std::lock_guard<std::mutex> invocationLock(invocationMutex_);
    if (shuttingDown_)
        return false;
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    ++activeInvocations_;
    connectAttempts_.push_back(attempt);
    return true;
}

void TcpRpcClient::endConnectInvocation(
    const std::shared_ptr<ConnectAttempt> &attempt)
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connectAttempts_.erase(
            std::remove(connectAttempts_.begin(), connectAttempts_.end(), attempt),
            connectAttempts_.end());
    }
    endInvocation();
}

TcpRpcClient::Result TcpRpcClient::connectInterrupted(
    const std::shared_ptr<ConnectAttempt> &attempt) const
{
    const int reason = attempt->interrupted.load();
    return {reason ? static_cast<Error>(reason) : Error::Disconnected,
            "connect interrupted"};
}

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
    ConnectInvocation invocation(this);
    if (!invocation)
        return {Error::Disconnected, "RPC client is shutting down"};
    const std::shared_ptr<ConnectAttempt> attempt = invocation.attempt();
#ifdef PBRPC_CLIENT_CORE_TESTING
    runConnectTestHook(ClientConnectTestPhase::ConnectAdmitted);
#endif
    std::lock_guard<std::mutex> callLock(callMutex_);
    const Clock::time_point deadline = Clock::now() + timeout;
    std::uint64_t operation;
    std::shared_ptr<Connection> old;
    {
        std::lock_guard<std::mutex> invocationLock(invocationMutex_);
        if (shuttingDown_)
            return {Error::Disconnected, "RPC client is shutting down"};
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (attempt->interrupted.load())
            return connectInterrupted(attempt);
        ++generation_;
        old.swap(connection_);
        if (old) {
            int expected = 0;
            old->interrupted.compare_exchange_strong(
                expected, static_cast<int>(Error::Disconnected));
        }
        operation = generation_;
    }
    if (old)
        ::shutdown(old->fd, SHUT_RDWR);
    addrinfo hints = {}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo *addresses = nullptr;
    const int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (generation_ != operation || attempt->interrupted.load()) {
            if (addresses)
                freeaddrinfo(addresses);
            return connectInterrupted(attempt);
        }
    }
    if (gai) return {Error::Transport, std::string("numeric IPv4/IPv6 address required: ") + gai_strerror(gai)};
    Result result{Error::Transport, "unable to connect"};
    for (addrinfo *it = addresses; it; it = it->ai_next) {
        { std::lock_guard<std::mutex> lock(stateMutex_); if (generation_ != operation) {
            result = connectInterrupted(attempt); break; } }
        if (Clock::now() >= deadline) { result = {Error::Timeout, "RPC operation timed out"}; break; }
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            if (attempt->interrupted.load()) {
                result = connectInterrupted(attempt);
                break;
            }
            result.message = std::strerror(errno);
            continue;
        }
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            if (attempt->interrupted.load())
                result = connectInterrupted(attempt);
            else
                result.message = std::strerror(errno);
            ::close(fd);
            if (attempt->interrupted.load())
                break;
            continue;
        }
        std::shared_ptr<Connection> candidate(new Connection(fd, operation));
        { std::lock_guard<std::mutex> lock(stateMutex_); if (generation_ != operation) {
            result = connectInterrupted(attempt); break; } connection_ = candidate; }
#ifdef PBRPC_CLIENT_CORE_TESTING
        runConnectTestHook(ClientConnectTestPhase::CandidatePublished);
#endif
        if (candidate->interrupted.load()) {
            result = connectInterrupted(attempt);
            break;
        }
#ifdef PBRPC_CLIENT_CORE_TESTING
        runConnectTestHook(ClientConnectTestPhase::BeforeSocketConnect);
#endif
        {
            std::lock_guard<std::mutex> invocationLock(invocationMutex_);
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            const int interrupted = candidate->interrupted.load();
            if (shuttingDown_ || connection_ != candidate || generation_ != operation
                    || interrupted) {
                result = connectInterrupted(attempt);
                break;
            }
        }
        bool socketConnected = false;
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0)
            socketConnected = true;
        else if (errno == EINPROGRESS) {
            result = wait(candidate, POLLOUT, deadline);
            if (result) {
                int error = 0; socklen_t length = sizeof(error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
                    result = fail(candidate, Error::Transport, std::strerror(error ? error : errno), true);
                else
                    socketConnected = true;
            }
        } else result = fail(candidate, Error::Transport, std::strerror(errno), true);
        const int interrupted = candidate->interrupted.load();
        if (!socketConnected && interrupted) {
            if (attempt->interrupted.load())
                result = connectInterrupted(attempt);
            break;
        }
        if (socketConnected) {
#ifdef PBRPC_CLIENT_CORE_TESTING
            runConnectTestHook(ClientConnectTestPhase::SocketConnected);
#endif
            std::lock_guard<std::mutex> lock(stateMutex_);
            const int interrupted = candidate->interrupted.load();
            if (connection_ != candidate || generation_ != operation || interrupted) {
                result = connectInterrupted(attempt);
            } else {
                candidate->state = Connection::Connected;
                result = {};
            }
        }
        if (result)
            break;
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
    Invocation invocation(this);
    if (!invocation)
        return {Error::Disconnected, "RPC client is shutting down"};
    struct Notice { std::uint16_t method; std::vector<std::uint8_t> payload; };
    std::vector<Notice> notices;
    std::unique_ptr<std::FILE, FileCloser> blobSpool(blobSink ? std::tmpfile() : nullptr);
    if (blobSink && !blobSpool) return {Error::Transport, "could not create binary blob spool"};
    Result result;
    {
        std::lock_guard<std::mutex> lock(callMutex_);
        std::shared_ptr<Connection> connection;
        { std::lock_guard<std::mutex> stateLock(stateMutex_); connection = connection_; }
        if (!connection || connection->state.load() != Connection::Connected)
            return {Error::Disconnected, "RPC client is not connected"};
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
            if (!accepted)
                return {Error::Protocol, "binary blob sink rejected data"};
        }
    }
    return result;
}

} // namespace pbrpc
