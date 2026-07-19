#include "client_session.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/util/message_differencer.h>

#include <algorithm>
#include <functional>
#include <sstream>
#include <utility>

namespace ostinato { namespace client {
namespace {

ClientSession::Result error(pbrpc::TcpRpcClient::Error code,
                            const std::string &message)
{
    return {code, message};
}

void setPort(OstProto::PortId *port, std::uint32_t id)
{
    port->set_id(id);
}

template <typename List, typename Id>
std::vector<std::uint32_t> idsFrom(const List &list, int count,
                                  Id idAt)
{
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < count; ++i)
        ids.push_back(idAt(list, i));
    return ids;
}

bool exactIds(std::vector<std::uint32_t> actual,
              std::vector<std::uint32_t> expected)
{
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected &&
           std::adjacent_find(actual.begin(), actual.end()) == actual.end();
}

std::vector<std::uint32_t> streamIds(const OstProto::StreamIdList &list)
{
    return idsFrom(list, list.stream_id_size(),
        [](const OstProto::StreamIdList &v, int i) {
            return v.stream_id(i).id();
        });
}

std::vector<std::uint32_t> groupIds(const OstProto::DeviceGroupIdList &list)
{
    return idsFrom(list, list.device_group_id_size(),
        [](const OstProto::DeviceGroupIdList &v, int i) {
            return v.device_group_id(i).id();
        });
}

template <typename Map>
std::vector<std::uint32_t> mapIds(const Map &values)
{
    std::vector<std::uint32_t> ids;
    for (const auto &value : values)
        ids.push_back(value.first);
    return ids;
}

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> callback)
        : callback_(std::move(callback)) {}
    ~ScopeExit() { callback_(); }

private:
    std::function<void()> callback_;
};

} // namespace

ClientSession::ClientSession()
{
    rpc_.setNotificationCallback(
        [this](std::uint16_t, const std::vector<std::uint8_t> &bytes) {
            OstProto::Notification notice;
            if (!notice.ParseFromArray(bytes.data(),
                                       static_cast<int>(bytes.size())) ||
                    notice.notif_type() != OstProto::portConfigChanged)
                return;
            std::lock_guard<std::mutex> lock(notificationMutex_);
            for (int i = 0; i < notice.port_id_list().port_id_size(); ++i)
                pendingNotifications_.insert(
                    notice.port_id_list().port_id(i).id());
        });
}

ClientSession::~ClientSession()
{
    disconnect();
}

ClientSession::Result ClientSession::call(
    const char *name, const google::protobuf::Message &request,
    google::protobuf::Message &response, std::chrono::milliseconds timeout,
    std::vector<std::uint8_t> *blob, BlobSink sink)
{
    const auto *method =
        OstProto::OstService::descriptor()->FindMethodByName(name);
    // Do not clear ports here. The caller may hold a PortState reference while
    // an RPC is interrupted by cancel()/disconnect(). Those public methods
    // take operationMutex_ as a barrier and clear state only after the active
    // operation has unwound.
    return rpc_.call(method, request, &response, timeout, blob,
                     std::move(sink));
}

void ClientSession::clearIfDisconnected()
{
    if (!rpc_.connected()) {
        ports_.clear();
        std::lock_guard<std::mutex> lock(notificationMutex_);
        pendingNotifications_.clear();
    }
}

bool ClientSession::connected() const
{
    std::lock_guard<std::recursive_mutex> lock(operationMutex_);
    return rpc_.connected();
}

ClientSession::Result ClientSession::connect(
    const std::string &host, std::uint16_t port, const std::string &version,
    std::chrono::milliseconds timeout)
{
    std::lock_guard<std::recursive_mutex> lock(operationMutex_);
    return connectUnlocked(host, port, version, timeout);
}

ClientSession::Result ClientSession::connectUnlocked(
    const std::string &host, std::uint16_t port, const std::string &version,
    std::chrono::milliseconds timeout)
{
    rpc_.disconnect();
    ports_.clear();
    host_ = host;
    endpointPort_ = port;
    version_ = version;
    Result result = rpc_.connect(host, port, timeout);
    if (!result)
        return result;

    OstProto::VersionInfo query;
    query.set_version(version);
    query.set_client_name("ostinato-headless");
    OstProto::VersionCompatibility answer;
    result = call("checkVersion", query, answer, timeout);
    if (!result || answer.result() != OstProto::VersionCompatibility::kCompatible) {
        if (result)
            result = error(pbrpc::TcpRpcClient::Error::Remote, answer.notes());
        rpc_.disconnect();
        clearIfDisconnected();
        return result;
    }
    result = hydrate(timeout);
    if (!result) {
        rpc_.disconnect();
        clearIfDisconnected();
    }
    return result;
}

ClientSession::Result ClientSession::reconnect(std::chrono::milliseconds timeout)
{
    std::lock_guard<std::recursive_mutex> lock(operationMutex_);
    return connectUnlocked(host_, endpointPort_, version_, timeout);
}

void ClientSession::disconnect()
{
    rpc_.disconnect();                 // Interrupt an active RPC first.
    std::lock_guard<std::recursive_mutex> lock(operationMutex_); // barrier
    clearIfDisconnected();
}

void ClientSession::cancel()
{
    rpc_.cancel();                     // Interrupt an active RPC first.
    std::lock_guard<std::recursive_mutex> lock(operationMutex_); // barrier
    clearIfDisconnected();
}

PortState *ClientSession::port(std::uint32_t id)
{
    auto found = ports_.find(id);
    return found == ports_.end() ? nullptr : &found->second;
}

ClientSession::Result ClientSession::hydrate(std::chrono::milliseconds timeout)
{
    OstProto::Void query;
    OstProto::PortIdList ids;
    Result result = call("getPortIdList", query, ids, timeout);
    if (!result)
        return result;
    std::vector<std::uint32_t> expected;
    for (int i = 0; i < ids.port_id_size(); ++i)
        expected.push_back(ids.port_id(i).id());
    if (!exactIds(expected, expected))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "duplicate port ID");

    OstProto::PortConfigList configs;
    result = call("getPortConfig", ids, configs, timeout);
    if (!result)
        return result;
    std::vector<std::uint32_t> actual;
    for (int i = 0; i < configs.port_size(); ++i)
        actual.push_back(configs.port(i).port_id().id());
    if (!exactIds(actual, expected))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "port configuration ID closure differs");

    std::map<std::uint32_t, PortState> staged;
    for (int i = 0; i < configs.port_size(); ++i) {
        const auto id = configs.port(i).port_id().id();
        result = hydratePort(id, configs.port(i), timeout);
        if (!result)
            return result;
        staged.emplace(id, std::move(ports_.at(id)));
        ports_.erase(id);
    }
    ports_.swap(staged);
    return {};
}

ClientSession::Result ClientSession::hydratePort(
    std::uint32_t id, const OstProto::Port &config,
    std::chrono::milliseconds timeout)
{
    PortState state(id);
    state.hydrateConfig(config);
    OstProto::PortId portId;
    portId.set_id(id);

    OstProto::DeviceGroupIdList groupIdList;
    Result result = call("getDeviceGroupIdList", portId, groupIdList, timeout);
    if (!result)
        return result;
    if (!groupIdList.has_port_id() || groupIdList.port_id().id() != id)
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "device-group ID response port differs");
    const auto groupsExpected = groupIds(groupIdList);
    if (!exactIds(groupsExpected, groupsExpected))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "duplicate device-group ID");

    OstProto::DeviceGroupConfigList groups;
    result = call("getDeviceGroupConfig", groupIdList, groups, timeout);
    if (!result)
        return result;
    std::vector<std::uint32_t> groupsActual;
    for (int i = 0; i < groups.device_group_size(); ++i)
        groupsActual.push_back(groups.device_group(i).device_group_id().id());
    if (!groups.has_port_id() || groups.port_id().id() != id ||
            !exactIds(groupsActual, groupsExpected))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "device-group configuration closure differs");
    state.hydrateDeviceGroups(groups);

    OstProto::PortDeviceList devices;
    result = call("getDeviceList", portId, devices, timeout);
    if (!result)
        return result;
    if (!devices.has_port_id() || devices.port_id().id() != id)
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "device response port differs");
    state.hydrateDevices(devices);

    OstProto::StreamIdList streamIdList;
    result = call("getStreamIdList", portId, streamIdList, timeout);
    if (!result)
        return result;
    if (!streamIdList.has_port_id() || streamIdList.port_id().id() != id)
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "stream ID response port differs");
    const auto streamsExpected = streamIds(streamIdList);
    if (!exactIds(streamsExpected, streamsExpected))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "duplicate stream ID");

    OstProto::StreamConfigList streams;
    result = call("getStreamConfig", streamIdList, streams, timeout);
    if (!result)
        return result;
    std::vector<std::uint32_t> streamsActual;
    for (int i = 0; i < streams.stream_size(); ++i)
        streamsActual.push_back(streams.stream(i).stream_id().id());
    if (!streams.has_port_id() || streams.port_id().id() != id ||
            !exactIds(streamsActual, streamsExpected))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "stream configuration closure differs");
    state.hydrateStreams(streams);
    state.markSyncComplete();
    ports_.erase(id);
    ports_.emplace(id, std::move(state));
    return {};
}

ClientSession::Result ClientSession::processNotifications(
    std::chrono::milliseconds timeout)
{
    std::set<std::uint32_t> ids;
    {
        std::lock_guard<std::mutex> lock(notificationMutex_);
        ids.swap(pendingNotifications_);
    }
    if (ids.empty())
        return {};

    OstProto::PortIdList query;
    for (auto id : ids)
        setPort(query.add_port_id(), id);
    OstProto::PortConfigList answer;
    Result result = call("getPortConfig", query, answer, timeout);
    std::vector<std::uint32_t> actual;
    for (int i = 0; result && i < answer.port_size(); ++i)
        actual.push_back(answer.port(i).port_id().id());
    const std::vector<std::uint32_t> expected(ids.begin(), ids.end());
    if (result && !exactIds(actual, expected))
        result = error(pbrpc::TcpRpcClient::Error::Protocol,
                       "notification configuration closure differs");
    if (!result) {
        std::lock_guard<std::mutex> lock(notificationMutex_);
        pendingNotifications_.insert(ids.begin(), ids.end());
        return result;
    }
    for (int i = 0; i < answer.port_size(); ++i) {
        auto found = ports_.find(answer.port(i).port_id().id());
        if (found != ports_.end())
            found->second.hydrateConfig(answer.port(i));
    }
    return {};
}

ClientSession::Result ClientSession::ackCall(
    const char *name, const google::protobuf::Message &query,
    std::chrono::milliseconds timeout)
{
    OstProto::Ack answer;
    Result result = call(name, query, answer, timeout);
    if (!result)
        return result;
    if (answer.status() != OstProto::Ack::kRpcSuccess)
        return error(pbrpc::TcpRpcClient::Error::Remote,
                     answer.notes().empty() ? std::string(name) + " failed"
                                            : answer.notes());
    return {};
}

ClientSession::Result ClientSession::apply(std::uint32_t id,
                                           std::chrono::milliseconds timeout)
{
    std::lock_guard<std::recursive_mutex> lock(operationMutex_);
    ScopeExit cleanup([this] { clearIfDisconnected(); });
    Result result = processNotifications(timeout);
    if (!result)
        return result;
    auto found = ports_.find(id);
    if (found == ports_.end())
        return error(pbrpc::TcpRpcClient::Error::Protocol, "unknown port");
    PortState &port = found->second;

    // Every attempt starts from server reality. This deliberately does not
    // clear desired dirty/modified state after a partially successful apply.
    OstProto::PortId pid;
    pid.set_id(id);
    OstProto::StreamIdList remoteStreams;
    result = call("getStreamIdList", pid, remoteStreams, timeout);
    if (!result)
        return result;
    OstProto::DeviceGroupIdList remoteGroups;
    result = call("getDeviceGroupIdList", pid, remoteGroups, timeout);
    if (!result)
        return result;
    if (!remoteStreams.has_port_id() || remoteStreams.port_id().id() != id ||
            !remoteGroups.has_port_id() || remoteGroups.port_id().id() != id)
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "apply baseline response port differs");
    const auto baselineStreams = streamIds(remoteStreams);
    const auto baselineGroups = groupIds(remoteGroups);
    if (!exactIds(baselineStreams, baselineStreams) ||
            !exactIds(baselineGroups, baselineGroups))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "duplicate apply baseline ID");
    port.syncState().refreshRemoteBaseline(baselineStreams, baselineGroups);

    const auto desiredStreams = mapIds(port.streams());
    const auto desiredGroups = mapIds(port.deviceGroups());
    const ApplyPlan plan = port.applyPlan();
    for (const auto &operation : plan.operations()) {
        if (operation.kind == ApplyOperation::DeleteDeviceGroups ||
                operation.kind == ApplyOperation::AddDeviceGroups) {
            OstProto::DeviceGroupIdList query;
            setPort(query.mutable_port_id(), id);
            const auto ids = operation.kind == ApplyOperation::DeleteDeviceGroups
                ? port.syncState().deletedDeviceGroups(desiredGroups)
                : port.syncState().newDeviceGroups(desiredGroups);
            for (auto value : ids)
                query.add_device_group_id()->set_id(value);
            result = ackCall(operation.kind == ApplyOperation::DeleteDeviceGroups
                                 ? "deleteDeviceGroup" : "addDeviceGroup",
                             query, timeout);
        } else if (operation.kind == ApplyOperation::ModifyDeviceGroups) {
            OstProto::DeviceGroupConfigList query;
            setPort(query.mutable_port_id(), id);
            for (auto value : port.syncState().modifiedDeviceGroups(desiredGroups))
                query.add_device_group()->CopyFrom(port.deviceGroups().at(value));
            result = ackCall("modifyDeviceGroup", query, timeout);
        } else if (operation.kind == ApplyOperation::RefreshDevices) {
            OstProto::PortDeviceList answer;
            result = call("getDeviceList", pid, answer, timeout);
            if (result && (!answer.has_port_id() || answer.port_id().id() != id))
                result = error(pbrpc::TcpRpcClient::Error::Protocol,
                               "refreshed device response port differs");
            if (result)
                port.hydrateDevices(answer);
        } else if (operation.kind == ApplyOperation::DeleteStreams ||
                   operation.kind == ApplyOperation::AddStreams) {
            OstProto::StreamIdList query;
            setPort(query.mutable_port_id(), id);
            const auto ids = operation.kind == ApplyOperation::DeleteStreams
                ? port.syncState().deletedStreams(desiredStreams)
                : port.syncState().newStreams(desiredStreams);
            for (auto value : ids)
                query.add_stream_id()->set_id(value);
            result = ackCall(operation.kind == ApplyOperation::DeleteStreams
                                 ? "deleteStream" : "addStream",
                             query, timeout);
        } else if (operation.kind == ApplyOperation::ModifyStreams) {
            OstProto::StreamConfigList query;
            setPort(query.mutable_port_id(), id);
            for (const auto &value : port.streams())
                query.add_stream()->CopyFrom(value.second);
            result = ackCall("modifyStream", query, timeout);
        } else if (operation.kind == ApplyOperation::ResolveNeighbors) {
            OstProto::PortIdList query;
            setPort(query.add_port_id(), id);
            result = ackCall("resolveDeviceNeighbors", query, timeout);
        } else {
            OstProto::BuildConfig query;
            setPort(query.mutable_port_id(), id);
            result = ackCall("build", query, timeout);
        }
        if (!result)
            return result;
    }

    OstProto::StreamIdList streamIdList;
    result = call("getStreamIdList", pid, streamIdList, timeout);
    if (!result)
        return result;
    OstProto::StreamConfigList streams;
    result = call("getStreamConfig", streamIdList, streams, timeout);
    if (!result)
        return result;
    OstProto::DeviceGroupIdList groupIdList;
    result = call("getDeviceGroupIdList", pid, groupIdList, timeout);
    if (!result)
        return result;
    OstProto::DeviceGroupConfigList groups;
    result = call("getDeviceGroupConfig", groupIdList, groups, timeout);
    if (!result)
        return result;

    const auto confirmedStreamIds = streamIds(streamIdList);
    const auto confirmedGroupIds = groupIds(groupIdList);
    std::vector<std::uint32_t> streamConfigIds, groupConfigIds;
    std::map<std::uint32_t, OstProto::Stream> confirmedStreams;
    for (int i = 0; i < streams.stream_size(); ++i) {
        const auto value = streams.stream(i).stream_id().id();
        streamConfigIds.push_back(value);
        confirmedStreams.emplace(value, streams.stream(i));
    }
    for (int i = 0; i < groups.device_group_size(); ++i)
        groupConfigIds.push_back(groups.device_group(i).device_group_id().id());
    if (!streamIdList.has_port_id() || streamIdList.port_id().id() != id ||
            !streams.has_port_id() || streams.port_id().id() != id ||
            !groupIdList.has_port_id() || groupIdList.port_id().id() != id ||
            !groups.has_port_id() || groups.port_id().id() != id ||
            !exactIds(confirmedStreamIds, desiredStreams) ||
            !exactIds(streamConfigIds, desiredStreams) ||
            !exactIds(confirmedGroupIds, desiredGroups) ||
            !exactIds(groupConfigIds, desiredGroups))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "server apply confirmation ID closure differs");
    for (const auto &desired : port.streams()) {
        if (!google::protobuf::util::MessageDifferencer::Equivalent(
                desired.second, confirmedStreams.at(desired.first)))
            return error(pbrpc::TcpRpcClient::Error::Protocol,
                         "server stream confirmation differs");
    }

    // Group defaults may be canonicalized by the server. Exact identity was
    // checked above, so hydrate canonical configurations before marking clean.
    port.hydrateDeviceGroups(groups);
    port.markSyncComplete();
    return {};
}

ClientSession::Result ClientSession::control(
    const char *name, std::uint32_t id, std::chrono::milliseconds timeout)
{
    std::lock_guard<std::recursive_mutex> lock(operationMutex_);
    ScopeExit cleanup([this] { clearIfDisconnected(); });
    Result result = processNotifications(timeout);
    if (!result)
        return result;
    if (!ports_.count(id))
        return error(pbrpc::TcpRpcClient::Error::Protocol, "unknown port");
    OstProto::PortIdList query;
    setPort(query.add_port_id(), id);
    return ackCall(name, query, timeout);
}

ClientSession::Result ClientSession::startTransmit(std::uint32_t id, std::chrono::milliseconds t) { return control("startTransmit", id, t); }
ClientSession::Result ClientSession::stopTransmit(std::uint32_t id, std::chrono::milliseconds t) { return control("stopTransmit", id, t); }
ClientSession::Result ClientSession::startCapture(std::uint32_t id, std::chrono::milliseconds t) { return control("startCapture", id, t); }
ClientSession::Result ClientSession::stopCapture(std::uint32_t id, std::chrono::milliseconds t) { return control("stopCapture", id, t); }

ClientSession::Result ClientSession::queryStats(std::chrono::milliseconds timeout)
{
    std::lock_guard<std::recursive_mutex> lock(operationMutex_);
    ScopeExit cleanup([this] { clearIfDisconnected(); });
    Result result = processNotifications(timeout);
    if (!result)
        return result;
    OstProto::PortIdList query;
    std::vector<std::uint32_t> expected;
    for (const auto &port : ports_) {
        setPort(query.add_port_id(), port.first);
        expected.push_back(port.first);
    }
    OstProto::PortStatsList answer;
    result = call("getStats", query, answer, timeout);
    if (!result)
        return result;
    std::vector<std::uint32_t> actual;
    for (int i = 0; i < answer.port_stats_size(); ++i)
        actual.push_back(answer.port_stats(i).port_id().id());
    if (!exactIds(actual, expected))
        return error(pbrpc::TcpRpcClient::Error::Protocol,
                     "statistics ID closure differs");
    for (int i = 0; i < answer.port_stats_size(); ++i)
        ports_.at(answer.port_stats(i).port_id().id()).hydrateStats(
            answer.port_stats(i));
    return {};
}

ClientSession::Result ClientSession::getCapture(
    std::uint32_t id, std::vector<std::uint8_t> &data,
    std::chrono::milliseconds timeout, BlobSink sink)
{
    std::lock_guard<std::recursive_mutex> lock(operationMutex_);
    ScopeExit cleanup([this] { clearIfDisconnected(); });
    Result result = processNotifications(timeout);
    if (!result)
        return result;
    if (!ports_.count(id))
        return error(pbrpc::TcpRpcClient::Error::Protocol, "unknown port");
    OstProto::PortId query;
    query.set_id(id);
    OstProto::CaptureBuffer answer;
    data.clear();
    return call("getCaptureBuffer", query, answer, timeout, &data,
                std::move(sink));
}

} } // namespace ostinato::client
