#ifndef OSTINATO_CLIENT_CORE_PORT_STATE_H
#define OSTINATO_CLIENT_CORE_PORT_STATE_H

#include "protocol.pb.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace ostinato { namespace client {

class PortSyncState {
public:
    const std::set<std::uint32_t> &syncedStreamIds() const { return streams_; }
    const std::set<std::uint32_t> &syncedDeviceGroupIds() const { return deviceGroups_; }
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markDeviceGroupModified(std::uint32_t id) { modifiedDeviceGroups_.insert(id); dirty_ = true; }
    std::vector<std::uint32_t> newStreams(const std::vector<std::uint32_t> &current) const;
    std::vector<std::uint32_t> deletedStreams(const std::vector<std::uint32_t> &current) const;
    std::vector<std::uint32_t> newDeviceGroups(const std::vector<std::uint32_t> &current) const;
    std::vector<std::uint32_t> deletedDeviceGroups(const std::vector<std::uint32_t> &current) const;
    std::vector<std::uint32_t> modifiedDeviceGroups(const std::vector<std::uint32_t> &current) const;
    // Update what is known to exist remotely without changing desired edits.
    void refreshRemoteBaseline(const std::vector<std::uint32_t> &streams,
                               const std::vector<std::uint32_t> &deviceGroups);
    void markSyncComplete(const std::vector<std::uint32_t> &streams,
                          const std::vector<std::uint32_t> &deviceGroups);
private:
    std::set<std::uint32_t> streams_, deviceGroups_, modifiedDeviceGroups_;
    bool dirty_ = false;
};

struct ApplyOperation {
    enum Kind { DeleteDeviceGroups, AddDeviceGroups, ModifyDeviceGroups,
                RefreshDevices, DeleteStreams, AddStreams, ModifyStreams,
                ResolveNeighbors, Build };
    Kind kind;
};

class ApplyPlan {
public:
    static ApplyPlan create(const std::map<std::uint32_t, OstProto::Stream> &streams,
                            const std::map<std::uint32_t, OstProto::DeviceGroup> &groups,
                            const PortSyncState &sync);
    const std::vector<ApplyOperation> &operations() const { return operations_; }
private:
    std::vector<ApplyOperation> operations_;
};

class PortState {
public:
    explicit PortState(std::uint32_t id = 0);
    std::uint32_t id() const { return id_; }
    const OstProto::Port &config() const { return config_; }
    const std::map<std::uint32_t, OstProto::Stream> &streams() const { return streams_; }
    const std::map<std::uint32_t, OstProto::DeviceGroup> &deviceGroups() const { return groups_; }
    const OstProto::PortDeviceList &devices() const { return devices_; }
    const OstProto::PortStats &stats() const { return stats_; }
    PortSyncState &syncState() { return sync_; }
    const PortSyncState &syncState() const { return sync_; }
    bool dirty() const { return sync_.dirty(); }
    bool transmitting() const { return transmitting_; }
    bool capturing() const { return capturing_; }

    void hydrateConfig(const OstProto::Port &value);
    void hydrateStreams(const OstProto::StreamConfigList &value);
    void hydrateDeviceGroups(const OstProto::DeviceGroupConfigList &value);
    void hydrateDevices(const OstProto::PortDeviceList &value) { devices_.CopyFrom(value); }
    void hydrateStats(const OstProto::PortStats &value);
    bool addStream(const OstProto::Stream &value);
    bool updateStream(const OstProto::Stream &value);
    bool deleteStream(std::uint32_t id);
    bool addDeviceGroup(const OstProto::DeviceGroup &value);
    bool updateDeviceGroup(const OstProto::DeviceGroup &value);
    bool deleteDeviceGroup(std::uint32_t id);
    ApplyPlan applyPlan() const { return ApplyPlan::create(streams_, groups_, sync_); }
    void markSyncComplete();
private:
    std::vector<std::uint32_t> streamIds() const;
    std::vector<std::uint32_t> groupIds() const;
    std::uint32_t id_;
    OstProto::Port config_;
    std::map<std::uint32_t, OstProto::Stream> streams_;
    std::map<std::uint32_t, OstProto::DeviceGroup> groups_;
    OstProto::PortDeviceList devices_;
    OstProto::PortStats stats_;
    PortSyncState sync_;
    bool transmitting_ = false, capturing_ = false;
};

// Import/export intentionally remains the responsibility of the Qt ostfile adapter.
} } // namespace ostinato::client
#endif
