#include "eth2.pb.h"
#include "ip4.pb.h"
#include "ip6.pb.h"
#include "mac.h"
#include "mac.pb.h"
#include "payload.pb.h"
#include "protocolmanager.h"
#include "protocollistiterator.h"
#include "streambase.h"
#include "tcp.pb.h"
#include "udp.pb.h"
#include "userscript.h"
#include "userscript.pb.h"
#include "vlan.pb.h"

#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

quint64 getDeviceMacAddress(int, int, int) { return 0; }
quint64 getNeighborMacAddress(int, int, int) { return 0; }
extern ProtocolManager *OstProtocolManager;

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string hex(const uchar *data, int size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < size; ++i)
        out << std::setw(2) << unsigned(data[i]);
    return out.str();
}

OstProto::Protocol *addProtocol(OstProto::Stream &stream, quint32 id)
{
    OstProto::Protocol *protocol = stream.add_protocol();
    protocol->mutable_protocol_id()->set_id(id);
    return protocol;
}

OstProto::Stream packetConfig(bool tcp, bool vlan, bool ipv6)
{
    OstProto::Stream config;
    config.mutable_stream_id()->set_id(1);
    config.mutable_core()->set_frame_len(ipv6 ? 82 : 64);
    OstProto::Mac *mac = addProtocol(config, OstProto::Protocol::kMacFieldNumber)
        ->MutableExtension(OstProto::mac);
    mac->set_dst_mac_mode(OstProto::Mac::e_mm_fixed);
    mac->set_dst_mac(0x001122334455ULL);
    mac->set_src_mac_mode(OstProto::Mac::e_mm_fixed);
    mac->set_src_mac(0x66778899aabbULL);
    if (vlan)
        addProtocol(config, OstProto::Protocol::kVlanFieldNumber)
            ->MutableExtension(OstProto::vlan)->set_vlan_tag(0xa02a);
    addProtocol(config, OstProto::Protocol::kEth2FieldNumber)
        ->MutableExtension(OstProto::eth2);
    if (ipv6) {
        OstProto::Ip6 *ip = addProtocol(config, OstProto::Protocol::kIp6FieldNumber)
            ->MutableExtension(OstProto::ip6);
        ip->set_hop_limit(64);
        ip->set_src_addr_hi(0x20010db800000000ULL); ip->set_src_addr_lo(1);
        ip->set_dst_addr_hi(0x20010db800000000ULL); ip->set_dst_addr_lo(2);
    } else {
        OstProto::Ip4 *ip = addProtocol(config, OstProto::Protocol::kIp4FieldNumber)
            ->MutableExtension(OstProto::ip4);
        ip->set_id(0x1234); ip->set_ttl(64);
        ip->set_src_ip(0xc0000201); ip->set_dst_ip(0xc6336402);
    }
    if (tcp) {
        OstProto::Tcp *p = addProtocol(config, OstProto::Protocol::kTcpFieldNumber)
            ->MutableExtension(OstProto::tcp);
        p->set_is_override_src_port(true); p->set_is_override_dst_port(true);
        p->set_src_port(12345); p->set_dst_port(443);
        p->set_seq_num(0x01020304); p->set_ack_num(0x05060708);
        p->set_flags(0x18); p->set_window(4096);
    } else {
        OstProto::Udp *p = addProtocol(config, OstProto::Protocol::kUdpFieldNumber)
            ->MutableExtension(OstProto::udp);
        p->set_is_override_src_port(true); p->set_is_override_dst_port(true);
        p->set_src_port(12345); p->set_dst_port(53);
    }
    OstProto::Payload *payload = addProtocol(config, OstProto::Protocol::kPayloadFieldNumber)
        ->MutableExtension(OstProto::payload);
    payload->set_pattern_mode(OstProto::Payload::e_dp_fixed_word);
    payload->set_pattern(0x01020304);
    return config;
}

void checkPacket(bool tcp, bool vlan, bool ipv6, const std::string &golden)
{
    StreamBase stream;
    stream.protoDataCopyFrom(packetConfig(tcp, vlan, ipv6));
    const int size = (ipv6 ? 82 : 64) - kFcsSize;
    std::vector<uchar> bytes(size);
    require(stream.frameValue(bytes.data(), size, 0) == size, "packet size");
    require(hex(bytes.data(), size) == golden, "packet bytes/checksums differ");
}

UserScriptProtocol *scriptProtocol(StreamBase &stream)
{
    ProtocolListIterator *it = stream.createProtocolListIterator();
    UserScriptProtocol *result = nullptr;
    while (it->hasNext()) {
        AbstractProtocol *p = it->next();
        if (p->protocolNumber() == OstProto::Protocol::kUserScriptFieldNumber)
            result = static_cast<UserScriptProtocol *>(p);
    }
    delete it;
    return result;
}

UserScriptProtocol *loadScript(StreamBase &stream, const char *program)
{
    OstProto::Stream config;
    config.mutable_stream_id()->set_id(3);
    config.mutable_core()->set_frame_len(64);
    addProtocol(config, OstProto::Protocol::kUserScriptFieldNumber)
        ->MutableExtension(OstProto::userScript)->set_program(program);
    addProtocol(config, OstProto::Protocol::kPayloadFieldNumber)
        ->MutableExtension(OstProto::payload);
    stream.protoDataCopyFrom(config);
    return scriptProtocol(stream);
}

void packets()
{
    checkPacket(false, false, false, "00112233445566778899aabb08004500002e1234000040117c54c0000201c633640230390035001ad1fa010203040102030401020304010203040102");
    checkPacket(true, false, false, "00112233445566778899aabb08004500002e1234000040067c5fc0000201c6336402303901bb0102030405060708501810006c7f0000010203040102");
    checkPacket(false, true, false, "00112233445566778899aabb8100a02a08004500002a1234000040117c58c0000201c6336402303900350016d6080102030401020304010203040102");
    checkPacket(false, false, true, "00112233445566778899aabb86dd600000000018114020010db800000000000000000000000120010db800000000000000000000000230390035001863c301020304010203040102030401020304");
}

void variablesReplaceAndRoundTrip()
{
    OstProto::Stream config = packetConfig(false, false, false);
    OstProto::Mac *mac = config.mutable_protocol(0)->MutableExtension(OstProto::mac);
    mac->set_dst_mac_mode(OstProto::Mac::e_mm_inc);
    mac->set_dst_mac_count(3);
    mac->set_dst_mac_step(2);
    StreamBase stream;
    stream.protoDataCopyFrom(config);
    require(stream.isFrameVariable() && stream.frameVariableCount() == 3,
            "variable field count");
    std::vector<uchar> bytes(60);
    stream.frameValue(bytes.data(), 60, 2);
    require(hex(bytes.data(), 6) == "001122334459", "variable MAC value");
    require(stream.protocolFieldReplace(OstProto::Protocol::kMacFieldNumber,
        MacProtocol::mac_srcAddr, 48, QVariant(quint64(0x66778899aabbULL)),
        QVariant(quint64(0xffffffffffffULL)), QVariant(quint64(0x102030405060ULL)),
        QVariant(quint64(0xffffffffffffULL))) == 1, "protocolFieldReplace count");
    OstProto::Stream restored;
    stream.protoDataCopyInto(restored);
    require(restored.protocol(0).GetExtension(OstProto::mac).src_mac()
        == 0x102030405060ULL, "replacement/protobuf round-trip");
    StreamBase again;
    again.protoDataCopyFrom(restored);
    OstProto::Stream twice;
    again.protoDataCopyInto(twice);
    require(restored.SerializeAsString() == twice.SerializeAsString(),
            "Stream protobuf round-trip");
}

void quickJs()
{
    StreamBase stream;
    UserScriptProtocol *script = loadScript(stream,
        "protocol.protocolFrameSizeVariable=true; protocol.protocolFrameVariableCount=3;"
        "protocol.protocolFrameValue=function(i){return [i,255,256,-1,1.9,'2'];};"
        "protocol.protocolFrameSize=function(i){return 6+i;};"
        "protocol.protocolFrameCksum=function(i){if(i===2)return; if(i===7)throw 7; return 0xabcd;};");
    require(script && script->isScriptValid(), "QuickJS script success");
    require(script->isProtocolFrameSizeVariable()
        && script->protocolFrameVariableCount() == 3
        && script->protocolFrameSize(2) == 8, "QuickJS variable size");
    require(script->fieldData(UserScriptProtocol::userScript_program,
        AbstractProtocol::FieldFrameValue, 2).toByteArray().toHex() == QByteArray("02ff00ff0102"),
        "QuickJS byte conversion");
    require(script->protocolFrameCksum(2, AbstractProtocol::CksumIp,
        AbstractProtocol::IncludeCksumField) == 0, "QuickJS undefined checksum");
    require(script->protocolFrameCksum(7, AbstractProtocol::CksumIp,
        AbstractProtocol::IncludeCksumField) == 7, "QuickJS checksum exception conversion");
    require(script->protocolFrameCksum(3, AbstractProtocol::CksumIp,
        AbstractProtocol::IncludeCksumField) == 0xabcd, "QuickJS recovery after error");

    StreamBase badStream;
    script = loadScript(badStream,
        "protocol.protocolFrameValue=function(){throw new Error('core failure');};"
        "protocol.protocolFrameSize=function(){return 0;};");
    require(script && !script->isScriptValid(), "QuickJS runtime error detection");
    require(script->userScriptErrorText() == QString("Error: core failure"),
            "QuickJS runtime error text");
}

} // namespace

int main()
{
    ProtocolManager manager;
    OstProtocolManager = &manager;
    const std::pair<const char *, std::function<void()> > tests[] = {
        {"golden packets", packets},
        {"variables, replacement, protobuf round-trip", variablesReplaceAndRoundTrip},
        {"QuickJS", quickJs},
    };
    int failed = 0;
    for (const auto &test : tests) {
        try { test.second(); std::cout << "PASS: " << test.first << '\n'; }
        catch (const std::exception &e) {
            ++failed; std::cerr << "FAIL: " << test.first << ": " << e.what() << '\n';
        }
    }
    OstProtocolManager = nullptr;
    std::cout << (3 - failed) << "/3 tests passed\n";
    return failed ? 1 : 0;
}
