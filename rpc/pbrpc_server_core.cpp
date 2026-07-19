#include "pbrpc_server_core.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace pbrpc {

void ServerController::Reset() { failed_ = disconnect_ = hasBlob_ = false; notifications_ = true; error_.clear(); blob_.clear(); }
bool ServerController::Failed() const { return failed_; }
std::string ServerController::ErrorText() const { return error_; }
void ServerController::StartCancel() {}
void ServerController::SetFailed(const std::string &reason) { failed_ = true; error_ = reason; }
bool ServerController::IsCanceled() const { return false; }
void ServerController::NotifyOnCancel(google::protobuf::Closure *) {}

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
        const int fd = fd_.exchange(-1);
        if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
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
    void error(std::uint16_t method, const std::string &text, bool disconnect = false) {
        writeFrame(MessageType::Error, method, text);
        if (disconnect) { const int fd = fd_; if (fd >= 0) ::shutdown(fd, SHUT_RDWR); }
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
            const auto &blob = controller.binaryBlob();
            const auto wire = encodeFrame(MessageType::BinaryBlob, id, ByteView(blob.data(), blob.size()));
            std::lock_guard<std::mutex> lock(writeMutex_);
            const int fd = fd_; if (fd >= 0) sendAll(fd, wire.data(), wire.size());
        } else if (!response->IsInitialized()) error(id, "RPC response missing required fields");
        else { std::string payload; response->SerializeToString(&payload); writeFrame(MessageType::Response, id, payload); }
        if (id == 15 && !controller.Failed()) { compatible_ = true; notifications_ = controller.notificationsEnabled(); }
        if (controller.disconnect()) { const int fd = fd_; if (fd >= 0) ::shutdown(fd, SHUT_RDWR); return false; }
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
        { const int fd = fd_.exchange(-1); if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); } }
        closed_ = true;
    }
    std::atomic<int> fd_;
    google::protobuf::Service *service_;
    FrameParser parser_;
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
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        auto connection = std::make_shared<Connection>(fd, service_, options_.maxPayload);
        { std::lock_guard<std::mutex> lock(connectionsMutex_); connections_.push_back(connection); }
        connection->start();
        removeClosedConnections();
    }
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
