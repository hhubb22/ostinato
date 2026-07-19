#include "pbrpc_client_core.h"
#include "pbrpc_server_core.h"
#include "pbrpc_server_test.pb.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
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
        CHECK(condition_.wait_for(lock, seconds(5),
            [this, expected] { return slowEntered_ >= expected; }));
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

class Gate {
public:
    void enterAndWait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        CHECK(condition_.wait_for(lock, seconds(5), [this] { return released_; }));
    }
    void arrive()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entered_ = true;
        condition_.notify_all();
    }
    void waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        CHECK(condition_.wait_for(lock, seconds(5), [this] { return entered_; }));
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

static void testConnectInterruption(std::uint16_t port,
                                    ClientConnectTestPhase phase,
                                    TcpRpcClient::Error expected,
                                    const std::function<void(TcpRpcClient &)> &interrupt)
{
    TcpRpcClient client;
    Gate gate;
    setClientConnectTestHook([&](ClientConnectTestPhase current) {
        if (current == phase)
            gate.enterAndWait();
    });
    TcpRpcClient::Result result;
    std::thread connector([&] {
        result = client.connect("127.0.0.1", port, seconds(2));
    });
    gate.waitUntilEntered();
    CHECK(!client.connected());
    interrupt(client);
    gate.release();
    connector.join();
    setClientConnectTestHook({});
    CHECK(result.error == expected);
    CHECK(!client.connected());
}

static std::unique_ptr<TcpRpcClient> connectedClient(std::uint16_t port)
{
    std::unique_ptr<TcpRpcClient> client(new TcpRpcClient);
    CHECK(client->connect("127.0.0.1", port, seconds(2)));
    CHECK(invoke(*client, 15, "version"));
    CHECK(invoke(*client, 16, "compatibility barrier"));
    return client;
}

static void testDestructorWaitsForNotification(TcpRpcServer &server)
{
    auto client = connectedClient(server.port());
    Gate callbackGate;
    client->setNotificationCallback(
        [&](std::uint16_t, const std::vector<std::uint8_t> &) {
            callbackGate.enterAndWait();
        });
    pbrpc_test::Data notice;
    notice.set_value("destructor notification");
    server.broadcastNotification(44, notice);
    TcpRpcClient *raw = client.get();
    TcpRpcClient::Result callResult;
    std::thread caller([&] { callResult = invoke(*raw, 16, "notification barrier"); });
    callbackGate.waitUntilEntered();
    Gate destructorStarted;
    setClientDestructorTestHook([&] { destructorStarted.arrive(); });
    std::promise<void> destroyed;
    auto destroyedFuture = destroyed.get_future();
    std::thread destroyer([&] { client.reset(); destroyed.set_value(); });
    destructorStarted.waitUntilEntered();
    CHECK(destroyedFuture.wait_for(milliseconds(30)) == std::future_status::timeout);
    callbackGate.release();
    CHECK(destroyedFuture.wait_for(seconds(5)) == std::future_status::ready);
    caller.join();
    destroyer.join();
    CHECK(callResult);
    setClientDestructorTestHook({});
}

static void testDestructorWaitsForBlobSink(std::uint16_t port)
{
    auto client = connectedClient(port);
    Gate sinkGate;
    TcpRpcClient *raw = client.get();
    pbrpc_test::Data request, response;
    request.set_value("large");
    TcpRpcClient::Result callResult;
    std::thread caller([&] {
        callResult = raw->call(method(18), request, &response, seconds(2), nullptr,
            [&](const std::uint8_t *, std::size_t) {
                sinkGate.enterAndWait();
                return true;
            });
    });
    sinkGate.waitUntilEntered();
    Gate destructorStarted;
    setClientDestructorTestHook([&] { destructorStarted.arrive(); });
    std::promise<void> destroyed;
    auto destroyedFuture = destroyed.get_future();
    std::thread destroyer([&] { client.reset(); destroyed.set_value(); });
    destructorStarted.waitUntilEntered();
    CHECK(destroyedFuture.wait_for(milliseconds(30)) == std::future_status::timeout);
    sinkGate.release();
    CHECK(destroyedFuture.wait_for(seconds(5)) == std::future_status::ready);
    caller.join();
    destroyer.join();
    CHECK(callResult);
    setClientDestructorTestHook({});
}

static void testDestructorRejectsQueuedConnect(Service &service, std::uint16_t port)
{
    auto client = connectedClient(port);
    TcpRpcClient *raw = client.get();
    TcpRpcClient::Result slowResult, connectResult;
    std::thread slow([&] { slowResult = invoke(*raw, 20, "hold call lock", seconds(10)); });
    service.waitForSlow();
    Gate admittedGate;
    setClientConnectTestHook([&](ClientConnectTestPhase phase) {
        if (phase == ClientConnectTestPhase::ConnectAdmitted)
            admittedGate.enterAndWait();
    });
    std::thread connector([&] {
        connectResult = raw->connect("127.0.0.1", port, seconds(2));
    });
    admittedGate.waitUntilEntered();
    Gate destructorGate;
    setClientDestructorTestHook([&] { destructorGate.enterAndWait(); });
    std::thread destroyer([&] { client.reset(); });
    destructorGate.waitUntilEntered();
    destructorGate.release();
    admittedGate.release();
    slow.join();
    connector.join();
    destroyer.join();
    setClientConnectTestHook({});
    setClientDestructorTestHook({});
    service.releaseSlow();
    CHECK(slowResult.error == TcpRpcClient::Error::Disconnected);
    CHECK(connectResult.error == TcpRpcClient::Error::Disconnected);
}

static void testCancelQueuedConnect(Service &service, std::uint16_t port)
{
    auto client = connectedClient(port);
    TcpRpcClient::Result slowResult, connectResult;
    std::thread slow([&] {
        slowResult = invoke(*client, 20, "hold call lock for cancel", seconds(10));
    });
    service.waitForSlow();
    Gate admittedGate;
    setClientConnectTestHook([&](ClientConnectTestPhase phase) {
        if (phase == ClientConnectTestPhase::ConnectAdmitted)
            admittedGate.enterAndWait();
    });
    std::thread connector([&] {
        connectResult = client->connect("127.0.0.1", port, seconds(2));
    });
    admittedGate.waitUntilEntered();
    client->cancel();
    CHECK(!client->connected());
    admittedGate.release();
    slow.join();
    connector.join();
    setClientConnectTestHook({});
    service.releaseSlow();
    CHECK(slowResult.error == TcpRpcClient::Error::Canceled);
    CHECK(connectResult.error == TcpRpcClient::Error::Canceled);
    CHECK(!client->connected());
}

static void testOldSinkCannotDisconnectNewGeneration(std::uint16_t port)
{
    auto client = connectedClient(port);
    Gate sinkGate;
    pbrpc_test::Data request, response;
    request.set_value("large");
    TcpRpcClient::Result rejected;
    std::thread caller([&] {
        rejected = client->call(method(18), request, &response, seconds(2), nullptr,
            [&](const std::uint8_t *, std::size_t) {
                sinkGate.enterAndWait();
                return false;
            });
    });
    sinkGate.waitUntilEntered();
    CHECK(client->connect("127.0.0.1", port, seconds(2)));
    CHECK(invoke(*client, 15, "new generation"));
    sinkGate.release();
    caller.join();
    CHECK(rejected.error == TcpRpcClient::Error::Protocol);
    CHECK(client->connected());
    CHECK(invoke(*client, 16, "new generation remains usable"));
}

int main()
{
    Service service;
    TcpRpcServer::Options options; options.address = "127.0.0.1";
    TcpRpcServer server(&service, options);
    std::string error; CHECK(server.start(&error));

    testConnectInterruption(server.port(), ClientConnectTestPhase::ConnectAdmitted,
                            TcpRpcClient::Error::Canceled,
                            [](TcpRpcClient &client) { client.cancel(); });
    testConnectInterruption(server.port(), ClientConnectTestPhase::ConnectAdmitted,
                            TcpRpcClient::Error::Disconnected,
                            [](TcpRpcClient &client) { client.disconnect(); });
    testConnectInterruption(server.port(), ClientConnectTestPhase::CandidatePublished,
                            TcpRpcClient::Error::Canceled,
                            [](TcpRpcClient &client) { client.cancel(); });
    testConnectInterruption(server.port(), ClientConnectTestPhase::CandidatePublished,
                            TcpRpcClient::Error::Disconnected,
                            [](TcpRpcClient &client) { client.disconnect(); });
    testConnectInterruption(server.port(), ClientConnectTestPhase::BeforeSocketConnect,
                            TcpRpcClient::Error::Canceled,
                            [](TcpRpcClient &client) { client.cancel(); });
    testConnectInterruption(server.port(), ClientConnectTestPhase::BeforeSocketConnect,
                            TcpRpcClient::Error::Disconnected,
                            [](TcpRpcClient &client) { client.disconnect(); });
    testConnectInterruption(server.port(), ClientConnectTestPhase::SocketConnected,
                            TcpRpcClient::Error::Canceled,
                            [](TcpRpcClient &client) { client.cancel(); });
    testConnectInterruption(server.port(), ClientConnectTestPhase::SocketConnected,
                            TcpRpcClient::Error::Disconnected,
                            [](TcpRpcClient &client) { client.disconnect(); });
    testDestructorWaitsForNotification(server);
    testDestructorWaitsForBlobSink(server.port());
    testDestructorRejectsQueuedConnect(service, server.port());
    testCancelQueuedConnect(service, server.port());
    testOldSinkCannotDisconnectNewGeneration(server.port());

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
    const std::uint16_t stoppedPort = server.port();
    server.stop();
    TcpRpcClient refusedClient;
    result = refusedClient.connect("127.0.0.1", stoppedPort, seconds(2));
    CHECK(result.error == TcpRpcClient::Error::Transport);
    std::cout << "pbrpc client core tests passed\n";
}
