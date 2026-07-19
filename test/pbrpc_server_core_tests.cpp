#include "pbrpc_server_core.h"
#include "pbrpc_server_test.pb.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace pbrpc;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed: " #condition << '\n'; std::exit(1); \
} } while (false)

class Service : public pbrpc_test::TestService {
public:
    void CallMethod(const google::protobuf::MethodDescriptor *method,
                    google::protobuf::RpcController *base,
                    const google::protobuf::Message *request,
                    google::protobuf::Message *response,
                    google::protobuf::Closure *done) override {
        auto *controller = static_cast<ServerController *>(base);
        const std::string value = static_cast<const pbrpc_test::Data *>(request)->value();
        if (method->index() == 17) controller->SetFailed("expected failure");
        else if (method->index() == 18) controller->setBinaryBlob({'b', 'l', 'o', 'b'});
        else {
            static_cast<pbrpc_test::Data *>(response)->set_value(value + " reply");
            if (method->index() == 19) controller->triggerDisconnect();
        }
        done->Run();
    }
};

static void sendAll(int fd, const std::vector<std::uint8_t> &wire, std::size_t begin = 0) {
    while (begin < wire.size()) { const ssize_t n = send(fd, wire.data() + begin, wire.size() - begin, MSG_NOSIGNAL); CHECK(n > 0); begin += n; }
}
static Frame receiveFrame(int fd) {
    FrameParser parser;
    std::uint8_t byte;
    while (!parser.hasFrame()) { CHECK(recv(fd, &byte, 1, 0) == 1); CHECK(parser.push(ByteView(&byte, 1))); }
    return parser.popFrame();
}
static std::vector<std::uint8_t> request(int method, const std::string &value) {
    pbrpc_test::Data data; data.set_value(value); std::string payload; CHECK(data.SerializeToString(&payload));
    return encodeFrame(MessageType::Request, method, ByteView(payload.data(), payload.size()));
}

int main() {
    Service service;
    TcpRpcServer::Options options; options.address = "127.0.0.1";
    TcpRpcServer server(&service, options);
    std::string error; CHECK(server.start(&error)); CHECK(server.port() != 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
    sockaddr_in addr = {}; addr.sin_family = AF_INET; addr.sin_port = htons(server.port()); inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

    auto wire = request(15, "version");
    CHECK(send(fd, wire.data(), 3, MSG_NOSIGNAL) == 3); sendAll(fd, wire, 3); // partial framing
    Frame frame = receiveFrame(fd); CHECK(frame.header.type == MessageType::Response && frame.header.method == 15);

    sendAll(fd, request(16, "hello")); frame = receiveFrame(fd);
    pbrpc_test::Data reply; CHECK(reply.ParseFromArray(frame.payload.data(), frame.payload.size())); CHECK(reply.value() == "hello reply");

    sendAll(fd, request(17, "x")); frame = receiveFrame(fd); CHECK(frame.header.type == MessageType::Error); CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "expected failure");
    sendAll(fd, request(18, "x")); frame = receiveFrame(fd); CHECK(frame.header.type == MessageType::BinaryBlob); CHECK(std::string(frame.payload.begin(), frame.payload.end()) == "blob");

    pbrpc_test::Data notification; notification.set_value("notice"); server.broadcastNotification(42, notification);
    frame = receiveFrame(fd); CHECK(frame.header.type == MessageType::Notification && frame.header.method == 42);

    sendAll(fd, request(19, "bye")); frame = receiveFrame(fd); CHECK(frame.header.type == MessageType::Response);
    char byte; CHECK(recv(fd, &byte, 1, 0) == 0); close(fd);
    server.stop(); CHECK(!server.running());
    std::cout << "pbrpc server core tests passed\n";
}
