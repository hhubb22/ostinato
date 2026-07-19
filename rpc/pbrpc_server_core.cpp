#include "pbrpc_server_core.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <limits>

namespace pbrpc {

ServerController::~ServerController() { finishBinaryBlob(); }
void ServerController::Reset() { finishBinaryBlob(); failed_ = disconnect_ = hasBlob_ = false; notifications_ = true; error_.clear(); blob_.clear(); blobSize_ = 0; blobReader_ = {}; }
bool ServerController::Failed() const { return failed_; }
std::string ServerController::ErrorText() const { return error_; }
void ServerController::StartCancel() {}
void ServerController::SetFailed(const std::string &reason) { failed_ = true; error_ = reason; }
bool ServerController::IsCanceled() const { return false; }
void ServerController::NotifyOnCancel(google::protobuf::Closure *) {}
void ServerController::setBinaryBlob(std::vector<std::uint8_t> blob)
{
    finishBinaryBlob();
    hasBlob_ = true;
    blobSize_ = 0;
    blobReader_ = {};
    blob_ = std::move(blob);
}
void ServerController::setBinaryBlobSource(std::uint64_t size, BlobReader reader,
                                           std::function<void()> finished)
{
    finishBinaryBlob();
    hasBlob_ = true;
    blob_.clear();
    blobSize_ = size;
    blobReader_ = std::move(reader);
    blobFinished_ = std::move(finished);
}
std::uint64_t ServerController::binaryBlobSize() const
{
    return blobReader_ ? blobSize_ : blob_.size();
}
bool ServerController::readBinaryBlob(std::uint8_t *data, std::size_t capacity,
                                      std::size_t &count)
{
    if (!blobReader_) return false;
    try { count = blobReader_(data, capacity); }
    catch (...) { count = 0; return false; }
    return count <= capacity;
}
void ServerController::finishBinaryBlob()
{
    if (blobFinished_) { auto finished = std::move(blobFinished_); finished(); }
}

namespace {
class Completion : public google::protobuf::Closure {
public:
    void Run() override { std::lock_guard<std::mutex> lock(mutex); done = true; cv.notify_one(); }
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

bool sendAll(int fd, const std::uint8_t *data, std::size_t size)
{
    while (size) {
        const ssize_t count = ::send(fd, data, size, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}
} // namespace

class TcpRpcServer::Connection {
public:
    Connection(int fd, google::protobuf::Service *service, std::uint32_t maxPayload)
        : fd_(fd), service_(service), parser_(maxPayload) {}
    ~Connection() { stop(); }
    void start() { thread_ = std::thread(&Connection::run, this); }
    void stop() {
        closeSocket();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }
    void closeSocket() {
        const int fd = fd_.exchange(-1);
        if (fd < 0) return;
        ::shutdown(fd, SHUT_RDWR);
        std::lock_guard<std::mutex> lock(writeMutex_);
        ::close(fd);
    }
    bool closed() const { return closed_; }
    void notify(std::uint16_t type, const google::protobuf::Message &message) {
        if (!compatible_ || !notifications_ || !message.IsInitialized()) return;
        std::string payload;
        if (message.SerializeToString(&payload)) writeFrame(MessageType::Notification, type, payload);
    }

private:
    bool writeFrame(MessageType type, std::uint16_t method, const std::string &payload) {
        const auto frame = encodeFrame(type, method, ByteView(payload.data(), payload.size()));
        std::lock_guard<std::mutex> lock(writeMutex_);
        const int fd = fd_;
        return fd >= 0 && sendAll(fd, frame.data(), frame.size());
    }
    bool writeBlob(std::uint16_t method, ServerController &controller) {
        const std::uint64_t size = controller.binaryBlobSize();
        if (size > parserMaxPayload_ || size > std::numeric_limits<std::uint32_t>::max()) return false;
        std::lock_guard<std::mutex> lock(writeMutex_);
        const int fd = fd_;
        if (fd < 0) return false;
        const auto header = encodeHeader(MessageType::BinaryBlob, method,
                                         static_cast<std::uint32_t>(size));
        if (!sendAll(fd, header.data(), header.size())) return false;
        if (!controller.binaryBlob().empty())
            return sendAll(fd, controller.binaryBlob().data(), controller.binaryBlob().size());
        std::uint8_t chunk[64 * 1024];
        std::uint64_t remaining = size;
        while (remaining) {
            std::size_t count = 0;
            const std::size_t wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, sizeof(chunk)));
            if (!controller.readBinaryBlob(chunk, wanted, count) || !count
                    || count > remaining || !sendAll(fd, chunk, count)) return false;
            remaining -= count;
        }
        return true;
    }
    void error(std::uint16_t method, const std::string &text, bool disconnect = false) {
        writeFrame(MessageType::Error, method, text);
        if (disconnect) closeSocket();
    }
    bool process(Frame frame) {
        const std::uint16_t id = frame.header.method;
        if (frame.header.type != MessageType::Request) { error(id, "unexpected msg type; expected 1"); return true; }
        if (!compatible_ && id != 15) { error(id, "version compatibility check pending", true); return false; }
        const auto *descriptor = service_->GetDescriptor();
        if (!descriptor || id >= descriptor->method_count()) { error(id, "invalid RPC method " + std::to_string(id)); return true; }
        const auto *method = descriptor->method(id);
        std::unique_ptr<google::protobuf::Message> request(service_->GetRequestPrototype(method).New());
        std::unique_ptr<google::protobuf::Message> response(service_->GetResponsePrototype(method).New());
        if (!request->ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size())) || !request->IsInitialized()) {
            error(id, "RPC " + method->name() + "() missing required fields in request - " + request->InitializationErrorString());
            return true;
        }
        ServerController controller;
        Completion completion;
        service_->CallMethod(method, &controller, request.get(), response.get(), &completion);
        { std::unique_lock<std::mutex> lock(completion.mutex); completion.cv.wait(lock, [&completion] { return completion.done; }); }
        if (controller.Failed()) writeFrame(MessageType::Error, id, controller.ErrorText());
        else if (controller.hasBinaryBlob()) {
            if (controller.binaryBlobSize() > parserMaxPayload_)
                error(id, "RPC binary blob exceeds configured maximum");
            else if (!writeBlob(id, controller)) { controller.finishBinaryBlob(); closeSocket(); return false; }
            controller.finishBinaryBlob();
        } else if (!response->IsInitialized()) error(id, "RPC response missing required fields");
        else { std::string payload; response->SerializeToString(&payload); writeFrame(MessageType::Response, id, payload); }
        if (id == 15 && !controller.Failed()) { compatible_ = true; notifications_ = controller.notificationsEnabled(); }
        if (controller.disconnect()) { closeSocket(); return false; }
        return true;
    }
    void run() {
        std::uint8_t buffer[8192];
        while (fd_ >= 0) {
            const ssize_t count = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 || !parser_.push(ByteView(buffer, static_cast<std::size_t>(count)))) break;
            while (parser_.hasFrame()) if (!process(parser_.popFrame())) goto finished;
        }
finished:
        closeSocket();
        closed_ = true;
    }
    std::atomic<int> fd_;
    google::protobuf::Service *service_;
    FrameParser parser_;
    const std::uint32_t parserMaxPayload_ = parser_.maxPayload();
    std::thread thread_;
    std::mutex writeMutex_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> compatible_{false};
    std::atomic<bool> notifications_{true};
};

TcpRpcServer::TcpRpcServer(google::protobuf::Service *service) : TcpRpcServer(service, Options()) {}
TcpRpcServer::TcpRpcServer(google::protobuf::Service *service, Options options) : service_(service), options_(std::move(options)) {}
TcpRpcServer::~TcpRpcServer() { stop(); }

bool TcpRpcServer::start(std::string *error)
{
    if (running_) return true;
    if (!service_) { if (error) *error = "protobuf service is null"; return false; }
    addrinfo hints = {}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    addrinfo *addresses = nullptr;
    const std::string portText = std::to_string(options_.port);
    const int rc = getaddrinfo(options_.address.empty() ? nullptr : options_.address.c_str(), portText.c_str(), &hints, &addresses);
    if (rc) { if (error) *error = gai_strerror(rc); return false; }
    for (addrinfo *it = addresses; it; it = it->ai_next) {
        listenFd_ = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (listenFd_ < 0) continue;
        int one = 1; setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(listenFd_, it->ai_addr, it->ai_addrlen) == 0 && ::listen(listenFd_, SOMAXCONN) == 0) break;
        ::close(listenFd_); listenFd_ = -1;
    }
    freeaddrinfo(addresses);
    if (listenFd_ < 0) { if (error) *error = std::strerror(errno); return false; }
    sockaddr_storage address = {}; socklen_t length = sizeof(address);
    if (getsockname(listenFd_, reinterpret_cast<sockaddr *>(&address), &length) == 0)
        boundPort_ = address.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port) : ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
    running_ = true;
    acceptThread_ = std::thread(&TcpRpcServer::acceptLoop, this);
    return true;
}

void TcpRpcServer::acceptLoop()
{
    while (running_) {
        pollfd event = {listenFd_, POLLIN, 0};
        const int ready = ::poll(&event, 1, 100);
        removeClosedConnections();
        if (!running_) break;
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (ready == 0) continue;
        if (!(event.revents & POLLIN)) break;
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        auto connection = std::make_shared<Connection>(fd, service_, options_.maxPayload);
        { std::lock_guard<std::mutex> lock(connectionsMutex_); connections_.push_back(connection); }
        connection->start();
        removeClosedConnections();
    }
}
std::size_t TcpRpcServer::connectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}
void TcpRpcServer::removeClosedConnections() {
    std::vector<std::shared_ptr<Connection>> dead;
    { std::lock_guard<std::mutex> lock(connectionsMutex_); auto it = connections_.begin(); while (it != connections_.end()) { if ((*it)->closed()) { dead.push_back(*it); it = connections_.erase(it); } else ++it; } }
}
void TcpRpcServer::stop() {
    if (!running_.exchange(false)) return;
    const int fd = listenFd_.exchange(-1); if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::shared_ptr<Connection>> connections;
    { std::lock_guard<std::mutex> lock(connectionsMutex_); connections.swap(connections_); }
    for (auto &connection : connections) connection->stop();
}
void TcpRpcServer::broadcastNotification(std::uint16_t type, const google::protobuf::Message &message) {
    std::vector<std::shared_ptr<Connection>> connections;
    { std::lock_guard<std::mutex> lock(connectionsMutex_); connections = connections_; }
    for (auto &connection : connections) connection->notify(type, message);
}

} // namespace pbrpc
