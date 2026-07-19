#include "client_session.h"
#include "emulproto.pb.h"
#include "pbrpc_server_core.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

using namespace ostinato::client;
using Error = pbrpc::TcpRpcClient::Error;

namespace {

const std::uint32_t kPort = 7;

void require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

OstProto::Stream stream(std::uint32_t id, const std::string &name)
{
    OstProto::Stream value;
    value.mutable_stream_id()->set_id(id);
    value.mutable_core()->set_name(name);
    return value;
}

OstProto::DeviceGroup group(std::uint32_t id, const std::string &name)
{
    OstProto::DeviceGroup value;
    value.mutable_device_group_id()->set_id(id);
    value.mutable_core()->set_name(name);
    return value;
}

class FakeService : public OstProto::OstService {
public:
    FakeService()
    {
        port.mutable_port_id()->set_id(kPort);
        port.set_name("fake0");
        streams.emplace(11, stream(11, "initial"));
        groups.emplace(21, group(21, "devices"));
        capture.resize(180000);
        for (std::size_t i = 0; i < capture.size(); ++i)
            capture[i] = static_cast<std::uint8_t>((i * 37) % 251);
    }

    void record(const char *name) { order.emplace_back(name); }
    void success(OstProto::Ack *ack, google::protobuf::Closure *done)
    {
        ack->set_status(OstProto::Ack::kRpcSuccess);
        done->Run();
    }
    void rejection(OstProto::Ack *ack, const std::string &message,
                   google::protobuf::Closure *done)
    {
        ++stateErrors;
        ack->set_status(OstProto::Ack::kRpcError);
        ack->set_notes(message);
        done->Run();
    }

    void checkVersion(google::protobuf::RpcController *,
                      const OstProto::VersionInfo *,
                      OstProto::VersionCompatibility *reply,
                      google::protobuf::Closure *done) override
    {
        record("checkVersion");
        reply->set_result(compatible ? OstProto::VersionCompatibility::kCompatible
                                     : OstProto::VersionCompatibility::kIncompatible);
        reply->set_notes("incompatible test version");
        done->Run();
    }

    void getPortIdList(google::protobuf::RpcController *, const OstProto::Void *,
                       OstProto::PortIdList *reply,
                       google::protobuf::Closure *done) override
    {
        record("getPortIdList");
        reply->add_port_id()->set_id(kPort);
        done->Run();
    }

    void getPortConfig(google::protobuf::RpcController *controller,
                       const OstProto::PortIdList *request,
                       OstProto::PortConfigList *reply,
                       google::protobuf::Closure *done) override
    {
        record("getPortConfig");
        if (failPortConfig) {
            failPortConfig = false;
            controller->SetFailed("deliberate notification refresh failure");
            done->Run();
            return;
        }
        for (int i = 0; i < request->port_id_size(); ++i)
            if (request->port_id(i).id() == kPort)
                reply->add_port()->CopyFrom(port);
        done->Run();
    }

    void getStreamIdList(google::protobuf::RpcController *,
                         const OstProto::PortId *request,
                         OstProto::StreamIdList *reply,
                         google::protobuf::Closure *done) override
    {
        record("getStreamIdList");
        reply->mutable_port_id()->CopyFrom(*request);
        for (const auto &value : streams)
            reply->add_stream_id()->set_id(value.first);
        done->Run();
    }

    void getStreamConfig(google::protobuf::RpcController *,
                         const OstProto::StreamIdList *request,
                         OstProto::StreamConfigList *reply,
                         google::protobuf::Closure *done) override
    {
        record("getStreamConfig");
        reply->mutable_port_id()->CopyFrom(request->port_id());
        for (int i = 0; i < request->stream_id_size(); ++i) {
            const auto found = streams.find(request->stream_id(i).id());
            if (found != streams.end())
                reply->add_stream()->CopyFrom(found->second);
        }
        if (corruptConfirmation && reply->stream_size()) {
            reply->mutable_stream(0)->mutable_core()->set_name("CORRUPTED");
            corruptConfirmation = false;
        }
        if (canonicalizeStreamDefaults && reply->stream_size())
            reply->mutable_stream(0)->mutable_core()->set_frame_len(64);
        done->Run();
    }

    void addStream(google::protobuf::RpcController *,
                   const OstProto::StreamIdList *request, OstProto::Ack *reply,
                   google::protobuf::Closure *done) override
    {
        record("addStream");
        for (int i = 0; i < request->stream_id_size(); ++i)
            if (streams.count(request->stream_id(i).id()))
                return rejection(reply, "duplicate stream", done);
        for (int i = 0; i < request->stream_id_size(); ++i)
            streams.emplace(request->stream_id(i).id(),
                            stream(request->stream_id(i).id(), "unconfigured"));
        success(reply, done);
    }

    void deleteStream(google::protobuf::RpcController *,
                      const OstProto::StreamIdList *request, OstProto::Ack *reply,
                      google::protobuf::Closure *done) override
    {
        record("deleteStream");
        for (int i = 0; i < request->stream_id_size(); ++i)
            if (!streams.count(request->stream_id(i).id()))
                return rejection(reply, "missing stream", done);
        for (int i = 0; i < request->stream_id_size(); ++i)
            streams.erase(request->stream_id(i).id());
        success(reply, done);
    }

    void modifyStream(google::protobuf::RpcController *,
                      const OstProto::StreamConfigList *request,
                      OstProto::Ack *reply,
                      google::protobuf::Closure *done) override
    {
        record("modifyStream");
        for (int i = 0; i < request->stream_size(); ++i)
            if (!streams.count(request->stream(i).stream_id().id()))
                return rejection(reply, "modify missing stream", done);
        for (int i = 0; i < request->stream_size(); ++i)
            streams[request->stream(i).stream_id().id()] = request->stream(i);
        success(reply, done);
    }

    void getDeviceGroupIdList(google::protobuf::RpcController *,
                              const OstProto::PortId *request,
                              OstProto::DeviceGroupIdList *reply,
                              google::protobuf::Closure *done) override
    {
        record("getDeviceGroupIdList");
        reply->mutable_port_id()->CopyFrom(*request);
        for (const auto &value : groups)
            reply->add_device_group_id()->set_id(value.first);
        done->Run();
    }

    void getDeviceGroupConfig(google::protobuf::RpcController *,
                              const OstProto::DeviceGroupIdList *request,
                              OstProto::DeviceGroupConfigList *reply,
                              google::protobuf::Closure *done) override
    {
        record("getDeviceGroupConfig");
        reply->mutable_port_id()->CopyFrom(request->port_id());
        for (int i = 0; i < request->device_group_id_size(); ++i) {
            const auto found = groups.find(request->device_group_id(i).id());
            if (found != groups.end())
                reply->add_device_group()->CopyFrom(found->second);
        }
        done->Run();
    }

    void addDeviceGroup(google::protobuf::RpcController *,
                        const OstProto::DeviceGroupIdList *request,
                        OstProto::Ack *reply,
                        google::protobuf::Closure *done) override
    {
        record("addDeviceGroup");
        for (int i = 0; i < request->device_group_id_size(); ++i)
            if (groups.count(request->device_group_id(i).id()))
                return rejection(reply, "duplicate group", done);
        for (int i = 0; i < request->device_group_id_size(); ++i)
            groups.emplace(request->device_group_id(i).id(),
                           group(request->device_group_id(i).id(), "unconfigured"));
        success(reply, done);
    }

    void deleteDeviceGroup(google::protobuf::RpcController *,
                           const OstProto::DeviceGroupIdList *request,
                           OstProto::Ack *reply,
                           google::protobuf::Closure *done) override
    {
        record("deleteDeviceGroup");
        for (int i = 0; i < request->device_group_id_size(); ++i)
            if (!groups.count(request->device_group_id(i).id()))
                return rejection(reply, "missing group", done);
        for (int i = 0; i < request->device_group_id_size(); ++i)
            groups.erase(request->device_group_id(i).id());
        success(reply, done);
    }

    void modifyDeviceGroup(google::protobuf::RpcController *,
                           const OstProto::DeviceGroupConfigList *request,
                           OstProto::Ack *reply,
                           google::protobuf::Closure *done) override
    {
        record("modifyDeviceGroup");
        if (failModifyDeviceGroup) {
            failModifyDeviceGroup = false;
            return rejection(reply, "deliberate group modify failure", done);
        }
        for (int i = 0; i < request->device_group_size(); ++i)
            if (!groups.count(request->device_group(i).device_group_id().id()))
                return rejection(reply, "modify missing group", done);
        for (int i = 0; i < request->device_group_size(); ++i) {
            auto canonical = request->device_group(i);
            canonical.mutable_core()->set_name(canonical.core().name() + "-canonical");
            groups[canonical.device_group_id().id()] = canonical;
        }
        success(reply, done);
    }

    void getDeviceList(google::protobuf::RpcController *,
                       const OstProto::PortId *request,
                       OstProto::PortDeviceList *reply,
                       google::protobuf::Closure *done) override
    {
        record("getDeviceList");
        reply->mutable_port_id()->CopyFrom(*request);
        auto *device = reply->AddExtension(OstEmul::device);
        device->set_mac(0x001122334455ULL);
        device->set_ip4(0xc0000201);
        done->Run();
    }

    void resolveDeviceNeighbors(google::protobuf::RpcController *,
                                const OstProto::PortIdList *, OstProto::Ack *reply,
                                google::protobuf::Closure *done) override
    {
        record("resolveDeviceNeighbors");
        success(reply, done);
    }

    void build(google::protobuf::RpcController *, const OstProto::BuildConfig *,
               OstProto::Ack *reply, google::protobuf::Closure *done) override
    {
        record("build");
        if (failBuild) {
            failBuild = false;
            reply->set_status(OstProto::Ack::kRpcError);
            reply->set_notes("late build failure");
            done->Run();
            return;
        }
        success(reply, done);
    }

    void control(const char *name, bool &state, bool value, OstProto::Ack *reply,
                 google::protobuf::Closure *done)
    {
        record(name);
        state = value;
        success(reply, done);
    }
#define CONTROL(method, member, value)                                         \
    void method(google::protobuf::RpcController *, const OstProto::PortIdList *, \
                OstProto::Ack *reply, google::protobuf::Closure *done) override \
    { control(#method, member, value, reply, done); }
    CONTROL(startTransmit, transmitting, true)
    CONTROL(stopTransmit, transmitting, false)
    CONTROL(startCapture, capturing, true)
    CONTROL(stopCapture, capturing, false)
#undef CONTROL

    void getStats(google::protobuf::RpcController *,
                  const OstProto::PortIdList *, OstProto::PortStatsList *reply,
                  google::protobuf::Closure *done) override
    {
        record("getStats");
        {
            std::unique_lock<std::mutex> lock(slowMutex);
            if (slowStats) {
                slowEntered = true;
                slowCv.notify_all();
                slowCv.wait(lock, [this] { return slowReleased; });
                slowStats = slowEntered = slowReleased = false;
            }
        }
        auto *stats = reply->add_port_stats();
        stats->mutable_port_id()->set_id(kPort);
        stats->set_rx_pkts(42);
        stats->mutable_state()->set_is_transmit_on(transmitting);
        stats->mutable_state()->set_is_capture_on(capturing);
        done->Run();
    }

    void getCaptureBuffer(google::protobuf::RpcController *controller,
                          const OstProto::PortId *, OstProto::CaptureBuffer *,
                          google::protobuf::Closure *done) override
    {
        record("getCaptureBuffer");
        static_cast<pbrpc::ServerController *>(controller)->setBinaryBlob(capture);
        done->Run();
    }

    void armSlowStats()
    {
        std::lock_guard<std::mutex> lock(slowMutex);
        slowStats = true;
        slowEntered = slowReleased = false;
    }
    void waitForSlowStats()
    {
        std::unique_lock<std::mutex> lock(slowMutex);
        slowCv.wait(lock, [this] { return slowEntered; });
    }
    void releaseSlowStats()
    {
        std::lock_guard<std::mutex> lock(slowMutex);
        slowReleased = true;
        slowCv.notify_all();
    }

    OstProto::Port port;
    std::map<std::uint32_t, OstProto::Stream> streams;
    std::map<std::uint32_t, OstProto::DeviceGroup> groups;
    std::vector<std::uint8_t> capture;
    std::vector<std::string> order;
    int stateErrors = 0;
    bool compatible = true, failBuild = false, corruptConfirmation = false;
    bool failModifyDeviceGroup = false, canonicalizeStreamDefaults = false;
    bool failPortConfig = false, transmitting = false, capturing = false;
    std::mutex slowMutex;
    std::condition_variable slowCv;
    bool slowStats = false, slowEntered = false, slowReleased = false;
};

std::vector<ApplyOperation::Kind> kinds(const ApplyPlan &plan)
{
    std::vector<ApplyOperation::Kind> result;
    for (const auto &operation : plan.operations())
        result.push_back(operation.kind);
    return result;
}

void characterizeApplyPlan()
{
    PortSyncState sync;
    sync.markSyncComplete({1, 2}, {3, 4});
    sync.markDeviceGroupModified(4);
    require(sync.newStreams({2, 5}) == std::vector<std::uint32_t>{5}, "new stream IDs");
    require(sync.deletedStreams({2, 5}) == std::vector<std::uint32_t>{1}, "deleted stream IDs");
    require(sync.newDeviceGroups({4, 6}) == std::vector<std::uint32_t>{6}, "new group IDs");
    require(sync.deletedDeviceGroups({4, 6}) == std::vector<std::uint32_t>{3}, "deleted group IDs");
    sync.markDirty();
    sync.refreshRemoteBaseline({2, 5}, {4, 6});
    require(sync.dirty() && sync.newStreams({2, 5}).empty(), "baseline preserves edits");

    std::map<std::uint32_t, OstProto::Stream> streams{{2, stream(2, "two")},
                                                      {7, stream(7, "seven")}};
    std::map<std::uint32_t, OstProto::DeviceGroup> groups{{4, group(4, "four")},
                                                          {8, group(8, "eight")}};
    sync.markSyncComplete({1, 2}, {3, 4});
    sync.markDeviceGroupModified(4);
    const std::vector<ApplyOperation::Kind> expected = {
        ApplyOperation::DeleteDeviceGroups, ApplyOperation::AddDeviceGroups,
        ApplyOperation::ModifyDeviceGroups, ApplyOperation::RefreshDevices,
        ApplyOperation::DeleteStreams, ApplyOperation::AddStreams,
        ApplyOperation::ModifyStreams, ApplyOperation::ResolveNeighbors,
        ApplyOperation::Build};
    require(kinds(ApplyPlan::create(streams, groups, sync)) == expected,
            "exact apply plan ordering");
}

void requireHydrated(ClientSession &client)
{
    auto *port = client.port(kPort);
    require(port && port->streams().count(11) && port->deviceGroups().count(21),
            "hydrated IDs");
    require(!port->dirty() && port->config().name() == "fake0", "hydrated clean config");
    require(port->devices().ExtensionSize(OstEmul::device) == 1, "real emulated device");
    require(port->devices().GetExtension(OstEmul::device, 0).mac() == 0x001122334455ULL,
            "emulated device contents");
}

} // namespace

int main()
{
    characterizeApplyPlan();
    FakeService fake;
    pbrpc::TcpRpcServer::Options options;
    options.address = "127.0.0.1";
    pbrpc::TcpRpcServer server(&fake, options);
    std::string serverError;
    require(server.start(&serverError), serverError);

    ClientSession client;
    const auto normal = std::chrono::seconds(2);
    require(static_cast<bool>(client.connect("127.0.0.1", server.port(), "test", normal)),
            "connect");
    const std::vector<std::string> hydration = {
        "checkVersion", "getPortIdList", "getPortConfig", "getDeviceGroupIdList",
        "getDeviceGroupConfig", "getDeviceList", "getStreamIdList", "getStreamConfig"};
    require(fake.order == hydration, "exact initial hydration order");
    requireHydrated(client);

    auto *port = client.port(kPort);
    require(port->deleteStream(11) && port->addStream(stream(12, "new stream")),
            "desired stream mutations");
    require(port->deleteDeviceGroup(21) && port->addDeviceGroup(group(22, "new group")),
            "desired group mutations");
    fake.order.clear();
    fake.failBuild = true;
    auto result = client.apply(kPort, normal);
    port = client.port(kPort);
    require(!result && result.error == Error::Remote && port->dirty(), "late build failure dirty");
    const std::vector<std::string> firstApply = {
        "getStreamIdList", "getDeviceGroupIdList", "deleteDeviceGroup", "addDeviceGroup",
        "modifyDeviceGroup", "getDeviceList", "deleteStream", "addStream", "modifyStream",
        "resolveDeviceNeighbors", "build"};
    require(fake.order == firstApply, "exact first apply ordering");
    require(fake.streams.count(12) && !fake.streams.count(11) && fake.groups.count(22) &&
                !fake.groups.count(21), "partial remote state after late failure");

    fake.order.clear();
    require(static_cast<bool>(client.apply(kPort, normal)), "retry apply");
    const std::vector<std::string> retry = {
        "getStreamIdList", "getDeviceGroupIdList", "modifyDeviceGroup", "getDeviceList", "modifyStream",
        "resolveDeviceNeighbors", "build", "getStreamIdList", "getStreamConfig",
        "getDeviceGroupIdList", "getDeviceGroupConfig"};
    require(fake.order == retry && fake.stateErrors == 0, "retry suppresses duplicate operations");
    port = client.port(kPort);
    require(!port->dirty() && port->deviceGroups().at(22).core().name() == "new group-canonical",
            "canonical group confirmation hydrated");

    require(port->addDeviceGroup(group(23, "retry group")), "new retry group");
    fake.failModifyDeviceGroup = true;
    fake.order.clear();
    result = client.apply(kPort, normal);
    port = client.port(kPort);
    require(!result && result.error == Error::Remote && port->dirty() &&
                fake.groups.at(23).core().name() == "unconfigured",
            "group add succeeds before modify failure");
    fake.order.clear();
    require(static_cast<bool>(client.apply(kPort, normal)), "group modify retry");
    port = client.port(kPort);
    require(std::find(fake.order.begin(), fake.order.end(), "addDeviceGroup") == fake.order.end() &&
                std::find(fake.order.begin(), fake.order.end(), "modifyDeviceGroup") != fake.order.end() &&
                port->deviceGroups().at(23).core().name() == "retry group-canonical" &&
                !port->dirty(), "retry resends desired group without duplicate add");

    auto changed = port->streams().at(12);
    changed.mutable_core()->set_name("semantic change");
    require(port->updateStream(changed), "semantic stream edit");
    fake.corruptConfirmation = true;
    result = client.apply(kPort, normal);
    port = client.port(kPort);
    require(!result && result.error == Error::Protocol && port->dirty(),
            "stream confirmation mismatch");
    fake.canonicalizeStreamDefaults = true;
    require(static_cast<bool>(client.apply(kPort, normal)),
            "equivalent canonical stream apply");
    port = client.port(kPort);
    require(!port->dirty() &&
                port->streams().at(12).core().has_frame_len() &&
                port->streams().at(12).core().frame_len() == 64,
            "equivalent canonical stream confirmation hydrated");

    require(client.startTransmit(kPort, normal) && client.startCapture(kPort, normal),
            "start controls");
    port = client.port(kPort);
    require(port->transmitting() && port->capturing(),
            "start controls update lifecycle before stats");
    require(client.queryStats(normal) && client.pollStats(normal), "repeated stats poll");
    port = client.port(kPort);
    require(port->stats().rx_pkts() == 42 && port->transmitting() && port->capturing(),
            "running stats state");
    require(client.stopTransmit(kPort, normal) && client.stopCapture(kPort, normal),
            "stop controls");
    port = client.port(kPort);
    require(!port->transmitting() && !port->capturing(),
            "stop controls update lifecycle before stats");
    require(static_cast<bool>(client.queryStats(normal)), "post-stop stats");
    port = client.port(kPort);
    require(port->stats().rx_pkts() == 42 && !port->transmitting() && !port->capturing(),
            "stopped stats state");

    std::vector<std::uint8_t> bytes;
    require(static_cast<bool>(client.startCapture(kPort, normal)), "capture restart");
    port = client.port(kPort);
    require(port->capturing(), "capture restart lifecycle");
    require(client.getCapture(kPort, bytes, normal) && bytes == fake.capture,
            "capture vector");
    port = client.port(kPort);
    require(!port->capturing(), "capture vector stops lifecycle");
    std::vector<std::uint8_t> sinkBytes, unused;
    std::size_t chunks = 0;
    require(client.getCapture(kPort, unused, normal,
                [&](const std::uint8_t *data, std::size_t size) {
                    ++chunks;
                    sinkBytes.insert(sinkBytes.end(), data, data + size);
                    return true;
                }) && sinkBytes == fake.capture && chunks > 1, "chunked capture sink");

    OstProto::Notification notice;
    notice.set_notif_type(OstProto::portConfigChanged);
    notice.mutable_port_id_list()->add_port_id()->set_id(kPort);
    fake.port.set_name("mismatched notification must be ignored");
    server.broadcastNotification(99, notice);
    require(client.queryStats(normal) && client.queryStats(normal),
            "mismatched notification calls complete");
    require(client.port(kPort)->config().name() != fake.port.name(),
            "wire/payload notification mismatch does not refresh");

    fake.port.set_name("renamed remotely");
    server.broadcastNotification(1, notice);
    fake.failPortConfig = true;
    require(static_cast<bool>(client.queryStats(normal)),
            "public call receives notification");
    result = client.queryStats(normal);
    require(!result && result.error == Error::Remote, "notification refresh failure");
    require(static_cast<bool>(client.queryStats(normal)), "retained notification retry");
    port = client.port(kPort);
    require(port->config().name() == "renamed remotely" && !port->dirty(),
            "notification refresh is metadata-only");

    result = client.startTransmit(999, normal);
    require(!result && result.error == Error::Protocol, "unknown port rejected");

    auto slowCase = [&](Error expected, bool timeout,
                        const std::function<void()> &interrupt) {
        fake.armSlowStats();
        ClientSession::Result outcome;
        std::thread worker([&] {
            outcome = client.queryStats(timeout ? std::chrono::milliseconds(30) : normal);
        });
        fake.waitForSlowStats();
        if (!timeout)
            interrupt();
        worker.join();
        require(!outcome && outcome.error == expected, "deterministic slow stats error");
        require(client.ports().empty(), "interrupted session ports clear");
        fake.releaseSlowStats();
        require(static_cast<bool>(client.reconnect(normal)),
                "reconnect after slow stats");
        require(client.port(kPort) != nullptr, "rehydration after slow stats");
    };
    slowCase(Error::Timeout, true, [] {});
    slowCase(Error::Canceled, false, [&] { client.cancel(); });
    slowCase(Error::Disconnected, false, [&] { client.disconnect(); });

    client.disconnect();
    fake.releaseSlowStats();
    server.stop();

    fake.compatible = false;
    pbrpc::TcpRpcServer incompatible(&fake, options);
    require(incompatible.start(&serverError), "start incompatible server");
    result = client.connect("127.0.0.1", incompatible.port(), "bad", normal);
    require(!result && result.error == Error::Remote && client.ports().empty(),
            "incompatible version rejected");
    client.disconnect();
    incompatible.stop();
    std::cout << "headless client E2E: PASS\n";
}
