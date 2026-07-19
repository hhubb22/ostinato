#include "pbrpc_client_core.h"
#include "pbrpc_server_core.h"
#include "pbrpc_server_test.pb.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

using namespace pbrpc;
using namespace std::chrono;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed: " #condition << '\n'; std::exit(1); \
} } while (false)

class Service : public pbrpc_test::TestService {
public:
    void CallMethod(const google::protobuf::MethodDescriptor *method,
                    google::protobuf::RpcController *base,
                    const google::protobuf::Message *request,
                    google::protobuf::Message *response,
                    google::protobuf::Closure *done) override
    {
        ServerController *controller = static_cast<ServerController *>(base);
        const std::string value = static_cast<const pbrpc_test::Data *>(request)->value();
        if (method->index() == 17) controller->SetFailed("expected failure");
        else if (method->index() == 18) {
            if (value == "large") controller->setBinaryBlob(std::vector<std::uint8_t>(300000, 'L'));
            else controller->setBinaryBlob({'b', 'l', 'o', 'b'});
        }
        else {
            if (method->index() == 20) {
                std::unique_lock<std::mutex> lock(mutex_);
                const unsigned ticket = ++slowEntered_; condition_.notify_all();
                condition_.wait(lock, [this, ticket] { return slowReleased_ >= ticket; });
            }
            static_cast<pbrpc_test::Data *>(response)->set_value(value + " reply");
            if (method->index() == 19) controller->triggerDisconnect();
        }
        done->Run();
    }
    void waitForSlow() {
        std::unique_lock<std::mutex> lock(mutex_);
        const unsigned expected = slowWaited_ + 1;
        condition_.wait(lock, [this, expected] { return slowEntered_ >= expected; });
        ++slowWaited_;
    }
    void releaseSlow() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++slowReleased_; condition_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    unsigned slowEntered_ = 0;
    unsigned slowWaited_ = 0;
    unsigned slowReleased_ = 0;
};

static const google::protobuf::MethodDescriptor *method(int index)
{
    return pbrpc_test::TestService::descriptor()->method(index);
}
static TcpRpcClient::Result invoke(TcpRpcClient &client, int index,
                                   const std::string &value,
                                   milliseconds timeout = seconds(2),
                                   std::vector<std::uint8_t> *blob = nullptr)
{
    pbrpc_test::Data request, response;
    request.set_value(value);
    TcpRpcClient::Result result = client.call(method(index), request, &response,
                                               timeout, blob);
    if (result && !blob) CHECK(response.value() == value + " reply");
    return result;
}

int main()
{
    Service service;
    TcpRpcServer::Options options; options.address = "127.0.0.1";
    TcpRpcServer server(&service, options);
    std::string error; CHECK(server.start(&error));

    TcpRpcClient client;
    CHECK(client.connect("127.0.0.1", server.port(), seconds(2)));
    CHECK(invoke(client, 15, "version"));
    CHECK(invoke(client, 16, std::string(50000, 'x'))); // spans many reads/writes

    TcpRpcClient::Result result = invoke(client, 17, "fail");
    CHECK(result.error == TcpRpcClient::Error::Remote && result.message == "expected failure");
    std::vector<std::uint8_t> blob;
    CHECK(invoke(client, 18, "blob", seconds(2), &blob));
    CHECK(std::string(blob.begin(), blob.end()) == "blob");

    pbrpc_test::Data largeRequest, largeResponse;
    largeRequest.set_value("large");
    std::size_t largeBytes = 0, sinkCalls = 0;
    CHECK(client.call(method(18), largeRequest, &largeResponse, seconds(2), nullptr,
        [&](const std::uint8_t *data, std::size_t size) {
            CHECK(size && data[0] == 'L'); largeBytes += size; ++sinkCalls; return true;
        }));
    CHECK(largeBytes == 300000 && sinkCalls > 1);

    std::mutex notificationMutex;
    std::condition_variable notificationCondition;
    bool notificationSeen = false;
    bool nestedCallSucceeded = false;
    client.setNotificationCallback([&](std::uint16_t type, const std::vector<std::uint8_t> &payload) {
        pbrpc_test::Data data;
        CHECK(type == 42 && data.ParseFromArray(payload.data(), payload.size()));
        CHECK(data.value() == "notice");
        nestedCallSucceeded = static_cast<bool>(invoke(client, 15, "nested notification call"));
        std::lock_guard<std::mutex> lock(notificationMutex);
        notificationSeen = true; notificationCondition.notify_one();
    });
    pbrpc_test::Data notice; notice.set_value("notice");
    server.broadcastNotification(42, notice);
    CHECK(invoke(client, 16, "after notification"));
    { std::unique_lock<std::mutex> lock(notificationMutex); CHECK(notificationSeen); }
    CHECK(nestedCallSucceeded);

    CHECK(invoke(client, 19, "bye"));
    for (int i = 0; i < 100 && client.connected(); ++i) {
        result = invoke(client, 16, "detect disconnect", milliseconds(100));
        if (!result) break;
    }
    CHECK(!result && !client.connected());
    CHECK(client.connect("127.0.0.1", server.port(), seconds(2)));
    CHECK(invoke(client, 15, "version"));

    std::thread timeoutWaiter([&] { service.waitForSlow(); });
    result = invoke(client, 20, "slow", milliseconds(80));
    timeoutWaiter.join();
    CHECK(result.error == TcpRpcClient::Error::Timeout && !client.connected());
    CHECK(client.connect("127.0.0.1", server.port(), seconds(2)));
    CHECK(invoke(client, 15, "version"));

    TcpRpcClient::Result canceled;
    std::thread caller([&] { canceled = invoke(client, 20, "cancel", seconds(10)); });
    service.waitForSlow();
    client.cancel();
    caller.join();
    CHECK(canceled.error == TcpRpcClient::Error::Canceled && !client.connected());

    service.releaseSlow();
    service.releaseSlow();
    CHECK(client.connect("127.0.0.1", server.port(), seconds(2)));
    CHECK(invoke(client, 15, "version"));

    TcpRpcClient::Result disconnected;
    std::thread disconnectCaller([&] { disconnected = invoke(client, 20, "disconnect", seconds(10)); });
    service.waitForSlow();
    client.disconnect();
    disconnectCaller.join();
    CHECK(disconnected.error == TcpRpcClient::Error::Disconnected && !client.connected());
    service.releaseSlow();
    CHECK(client.connect("127.0.0.1", server.port(), seconds(2)));
    CHECK(invoke(client, 15, "after disconnect"));
    client.disconnect(); CHECK(!client.connected());
    server.stop();
    std::cout << "pbrpc client core tests passed\n";
}
