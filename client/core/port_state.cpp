#include "port_state.h"

#include <algorithm>

namespace ostinato { namespace client {
namespace {
std::set<std::uint32_t> asSet(const std::vector<std::uint32_t> &v)
{
    return {v.begin(), v.end()};
}

std::vector<std::uint32_t> difference(const std::set<std::uint32_t> &a,
                                      const std::set<std::uint32_t> &b)
{
    std::vector<std::uint32_t> out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
    return out;
}

template <class Map>
std::vector<std::uint32_t> keys(const Map &values)
{
    std::vector<std::uint32_t> result;
    for (const auto &value : values)
        result.push_back(value.first);
    return result;
}
} // namespace

std::vector<std::uint32_t> PortSyncState::newStreams(
    const std::vector<std::uint32_t> &current) const
{
    return difference(asSet(current), streams_);
}

std::vector<std::uint32_t> PortSyncState::deletedStreams(
    const std::vector<std::uint32_t> &current) const
{
    return difference(streams_, asSet(current));
}

std::vector<std::uint32_t> PortSyncState::newDeviceGroups(
    const std::vector<std::uint32_t> &current) const
{
    return difference(asSet(current), deviceGroups_);
}

std::vector<std::uint32_t> PortSyncState::deletedDeviceGroups(
    const std::vector<std::uint32_t> &current) const
{
    return difference(deviceGroups_, asSet(current));
}

std::vector<std::uint32_t> PortSyncState::modifiedDeviceGroups(
    const std::vector<std::uint32_t> &currentIds) const
{
    std::vector<std::uint32_t> out;
    const auto current = asSet(currentIds);
    const auto added = newDeviceGroups(currentIds);
    std::set<std::uint32_t> modify(modifiedDeviceGroups_);
    modify.insert(added.begin(), added.end());
    for (auto id : modify)
        if (current.count(id))
            out.push_back(id);
    return out;
}
void PortSyncState::refreshRemoteBaseline(const std::vector<std::uint32_t> &s,
                                          const std::vector<std::uint32_t> &g)
{
    streams_ = asSet(s);
    deviceGroups_ = asSet(g);
}

void PortSyncState::markDeviceGroupsAddedRemote(
    const std::vector<std::uint32_t> &ids)
{
    deviceGroups_.insert(ids.begin(), ids.end());
}

void PortSyncState::markSyncComplete(const std::vector<std::uint32_t> &s,
                                     const std::vector<std::uint32_t> &g)
{
    refreshRemoteBaseline(s, g);
    modifiedDeviceGroups_.clear();
    dirty_ = false;
}

ApplyPlan ApplyPlan::create(
    const std::map<std::uint32_t, OstProto::Stream> &streams,
    const std::map<std::uint32_t, OstProto::DeviceGroup> &groups,
    const PortSyncState &sync)
{
    ApplyPlan plan;
    const auto streamIds = keys(streams);
    const auto groupIds = keys(groups);
    bool devicesChanged = false;
    if (!sync.deletedDeviceGroups(groupIds).empty()) {
        plan.operations_.push_back({ApplyOperation::DeleteDeviceGroups});
        devicesChanged = true;
    }
    if (!sync.newDeviceGroups(groupIds).empty()) {
        plan.operations_.push_back({ApplyOperation::AddDeviceGroups});
        devicesChanged = true;
    }
    if (!sync.modifiedDeviceGroups(groupIds).empty()) {
        plan.operations_.push_back({ApplyOperation::ModifyDeviceGroups});
        devicesChanged = true;
    }
    if (devicesChanged)
        plan.operations_.push_back({ApplyOperation::RefreshDevices});
    if (!sync.deletedStreams(streamIds).empty())
        plan.operations_.push_back({ApplyOperation::DeleteStreams});
    if (!sync.newStreams(streamIds).empty())
        plan.operations_.push_back({ApplyOperation::AddStreams});
    if (!streams.empty())
        plan.operations_.push_back({ApplyOperation::ModifyStreams});
    plan.operations_.push_back({ApplyOperation::ResolveNeighbors});
    plan.operations_.push_back({ApplyOperation::Build});
    return plan;
}

PortState::PortState(std::uint32_t id) : id_(id)
{
    config_.mutable_port_id()->set_id(id);
    devices_.mutable_port_id()->set_id(id);
    stats_.mutable_port_id()->set_id(id);
}

void PortState::hydrateConfig(const OstProto::Port &value)
{
    config_.CopyFrom(value);
}

void PortState::hydrateStreams(const OstProto::StreamConfigList &values)
{
    streams_.clear();
    for (int i = 0; i < values.stream_size(); ++i)
        streams_[values.stream(i).stream_id().id()] = values.stream(i);
}

void PortState::hydrateDeviceGroups(
    const OstProto::DeviceGroupConfigList &values)
{
    groups_.clear();
    for (int i = 0; i < values.device_group_size(); ++i)
        groups_[values.device_group(i).device_group_id().id()] =
            values.device_group(i);
}

void PortState::hydrateStats(const OstProto::PortStats &value)
{
    stats_.CopyFrom(value);
    transmitting_ = value.has_state() && value.state().is_transmit_on();
    capturing_ = value.has_state() && value.state().is_capture_on();
}

bool PortState::addStream(const OstProto::Stream &value)
{
    const auto id = value.stream_id().id();
    if (streams_.count(id))
        return false;
    streams_[id] = value;
    sync_.markDirty();
    return true;
}

bool PortState::updateStream(const OstProto::Stream &value)
{
    const auto id = value.stream_id().id();
    if (!streams_.count(id))
        return false;
    streams_[id] = value;
    sync_.markDirty();
    return true;
}

bool PortState::deleteStream(std::uint32_t id)
{
    if (!streams_.erase(id))
        return false;
    sync_.markDirty();
    return true;
}

bool PortState::addDeviceGroup(const OstProto::DeviceGroup &value)
{
    const auto id = value.device_group_id().id();
    if (groups_.count(id))
        return false;
    groups_[id] = value;
    // Adding the remote shell and configuring it are separate RPCs. Keep the
    // desired group marked modified until canonical readback succeeds so a
    // retry after modifyDeviceGroup failure resends its complete contents.
    sync_.markDeviceGroupModified(id);
    return true;
}

bool PortState::updateDeviceGroup(const OstProto::DeviceGroup &value)
{
    const auto id = value.device_group_id().id();
    if (!groups_.count(id))
        return false;
    groups_[id] = value;
    sync_.markDeviceGroupModified(id);
    return true;
}

bool PortState::deleteDeviceGroup(std::uint32_t id)
{
    if (!groups_.erase(id))
        return false;
    sync_.markDirty();
    return true;
}

std::vector<std::uint32_t> PortState::streamIds() const
{
    return keys(streams_);
}

std::vector<std::uint32_t> PortState::groupIds() const
{
    return keys(groups_);
}

void PortState::markSyncComplete()
{
    sync_.markSyncComplete(streamIds(), groupIds());
}

} } // namespace ostinato::client
