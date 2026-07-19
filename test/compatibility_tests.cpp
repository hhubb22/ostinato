#include "crc32c.h"
#include "eth2.pb.h"
#include "ip4.pb.h"
#include "ip6.pb.h"
#include "mac.pb.h"
#include "ostmfileformat.h"
#include "ossnfileformat.h"
#include "payload.pb.h"
#include "pbrpccommon.h"
#include "pbrpcchannel.h"
#include "pbrpccontroller.h"
#include "rpcserver.h"
#include "versioncompatibility.h"
#include "protocolmanager.h"
#include "protocollistiterator.h"
#include "streambase.h"
#include "tcp.pb.h"
#include "udp.pb.h"
#include "userscript.h"
#include "userscript.pb.h"
#include "vlan.pb.h"

#include <QFile>
#include <QBuffer>
#include <QHostAddress>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <google/protobuf/stubs/callback.h>

#include <cstring>

extern ProtocolManager *OstProtocolManager;

const char *version = "test";
const char *revision = "test";

quint64 getDeviceMacAddress(int, int, int)
{
    return 0;
}

quint64 getNeighborMacAddress(int, int, int)
{
    return 0;
}

namespace {

class TestClosure : public ::google::protobuf::Closure
{
public:
    explicit TestClosure(bool *called) : called_(called) {}
    void Run() override { *called_ = true; }
private:
    bool *called_;
};

class LoopbackService : public OstProto::OstService
{
public:
    void checkVersion(::google::protobuf::RpcController *controller,
        const OstProto::VersionInfo *request,
        OstProto::VersionCompatibility *response,
        ::google::protobuf::Closure *done) override
    {
        const VersionCompatibilityResult result = checkVersionCompatibility(
            QString::fromStdString(request->version()), "1.2.9");
        if (result == VersionInvalid)
            controller->SetFailed("invalid version information");
        else
            response->set_result(result == VersionCompatible
                ? OstProto::VersionCompatibility::kCompatible
                : OstProto::VersionCompatibility::kIncompatible);
        done->Run();
    }

    void getPortIdList(::google::protobuf::RpcController *,
        const OstProto::Void *, OstProto::PortIdList *response,
        ::google::protobuf::Closure *done) override
    {
        response->add_port_id()->set_id(42);
        done->Run();
    }
};

OstProto::Protocol *addProtocol(OstProto::Stream &stream, quint32 id)
{
    OstProto::Protocol *protocol = stream.add_protocol();
    protocol->mutable_protocol_id()->set_id(id);
    return protocol;
}

QByteArray packetBytes(bool tcp, bool vlan = false)
{
    OstProto::Stream config;
    config.mutable_stream_id()->set_id(1);
    config.mutable_core()->set_frame_len(64);

    OstProto::Mac *mac = addProtocol(
        config, OstProto::Protocol::kMacFieldNumber)->MutableExtension(
            OstProto::mac);
    mac->set_dst_mac_mode(OstProto::Mac::e_mm_fixed);
    mac->set_dst_mac(0x001122334455ULL);
    mac->set_src_mac_mode(OstProto::Mac::e_mm_fixed);
    mac->set_src_mac(0x66778899aabbULL);

    if (vlan) {
        OstProto::Vlan *tag = addProtocol(
            config, OstProto::Protocol::kVlanFieldNumber)->MutableExtension(
                OstProto::vlan);
        tag->set_vlan_tag(0xa02a);
    }
    addProtocol(config, OstProto::Protocol::kEth2FieldNumber)
        ->MutableExtension(OstProto::eth2);

    OstProto::Ip4 *ip = addProtocol(
        config, OstProto::Protocol::kIp4FieldNumber)->MutableExtension(
            OstProto::ip4);
    ip->set_id(0x1234);
    ip->set_ttl(64);
    ip->set_src_ip(0xc0000201);
    ip->set_dst_ip(0xc6336402);

    if (tcp) {
        OstProto::Tcp *transport = addProtocol(
            config, OstProto::Protocol::kTcpFieldNumber)->MutableExtension(
                OstProto::tcp);
        transport->set_is_override_src_port(true);
        transport->set_is_override_dst_port(true);
        transport->set_src_port(12345);
        transport->set_dst_port(443);
        transport->set_seq_num(0x01020304);
        transport->set_ack_num(0x05060708);
        transport->set_flags(0x18);
        transport->set_window(4096);
    } else {
        OstProto::Udp *transport = addProtocol(
            config, OstProto::Protocol::kUdpFieldNumber)->MutableExtension(
                OstProto::udp);
        transport->set_is_override_src_port(true);
        transport->set_is_override_dst_port(true);
        transport->set_src_port(12345);
        transport->set_dst_port(53);
    }

    OstProto::Payload *payload = addProtocol(
        config, OstProto::Protocol::kPayloadFieldNumber)->MutableExtension(
            OstProto::payload);
    payload->set_pattern_mode(OstProto::Payload::e_dp_fixed_word);
    payload->set_pattern(0x01020304);

    StreamBase stream;
    stream.protoDataCopyFrom(config);
    QByteArray bytes(60, 0);
    const int size = stream.frameValue(
        reinterpret_cast<uchar *>(bytes.data()), bytes.size(), 0);
    if (size != bytes.size())
        return QByteArray();
    return bytes;
}

QByteArray ipv6UdpPacketBytes()
{
    OstProto::Stream config;
    config.mutable_stream_id()->set_id(2);
    config.mutable_core()->set_frame_len(82);

    OstProto::Mac *mac = addProtocol(
        config, OstProto::Protocol::kMacFieldNumber)->MutableExtension(
            OstProto::mac);
    mac->set_dst_mac_mode(OstProto::Mac::e_mm_fixed);
    mac->set_dst_mac(0x001122334455ULL);
    mac->set_src_mac_mode(OstProto::Mac::e_mm_fixed);
    mac->set_src_mac(0x66778899aabbULL);
    addProtocol(config, OstProto::Protocol::kEth2FieldNumber)
        ->MutableExtension(OstProto::eth2);

    OstProto::Ip6 *ip = addProtocol(
        config, OstProto::Protocol::kIp6FieldNumber)->MutableExtension(
            OstProto::ip6);
    ip->set_hop_limit(64);
    ip->set_src_addr_hi(0x20010db800000000ULL);
    ip->set_src_addr_lo(1);
    ip->set_dst_addr_hi(0x20010db800000000ULL);
    ip->set_dst_addr_lo(2);

    OstProto::Udp *udp = addProtocol(
        config, OstProto::Protocol::kUdpFieldNumber)->MutableExtension(
            OstProto::udp);
    udp->set_is_override_src_port(true);
    udp->set_is_override_dst_port(true);
    udp->set_src_port(12345);
    udp->set_dst_port(53);

    OstProto::Payload *payload = addProtocol(
        config, OstProto::Protocol::kPayloadFieldNumber)->MutableExtension(
            OstProto::payload);
    payload->set_pattern_mode(OstProto::Payload::e_dp_fixed_word);
    payload->set_pattern(0x01020304);

    StreamBase stream;
    stream.protoDataCopyFrom(config);
    QByteArray bytes(78, 0);
    const int size = stream.frameValue(
        reinterpret_cast<uchar *>(bytes.data()), bytes.size(), 0);
    if (size != bytes.size())
        return QByteArray();
    return bytes;
}

UserScriptProtocol *userScriptProtocol(StreamBase &stream)
{
    ProtocolListIterator *iterator = stream.createProtocolListIterator();
    UserScriptProtocol *result = nullptr;
    while (iterator->hasNext()) {
        AbstractProtocol *protocol = iterator->next();
        if (protocol->protocolNumber()
                == OstProto::Protocol::kUserScriptFieldNumber) {
            result = static_cast<UserScriptProtocol *>(protocol);
            break;
        }
    }
    delete iterator;
    return result;
}

UserScriptProtocol *loadUserScript(StreamBase &stream, const QString &program)
{
    OstProto::Stream config;
    config.mutable_stream_id()->set_id(3);
    config.mutable_core()->set_frame_len(64);
    OstProto::UserScript *script = addProtocol(
        config, OstProto::Protocol::kUserScriptFieldNumber)->MutableExtension(
            OstProto::userScript);
    script->set_program(program.toStdString());
    addProtocol(config, OstProto::Protocol::kPayloadFieldNumber)
        ->MutableExtension(OstProto::payload);
    stream.protoDataCopyFrom(config);
    return userScriptProtocol(stream);
}

QByteArray rpcFrame(quint16 type, quint16 method, const QByteArray &payload)
{
    QByteArray frame(PB_HDR_SIZE, 0);
    pbRpcEncodeHeader(frame.data(), type, method, quint32(payload.size()));
    frame.append(payload);
    return frame;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

void refreshFileChecksum(QByteArray &bytes)
{
    QVERIFY(bytes.size() >= 5);
    std::memset(bytes.data() + bytes.size() - 4, 0, 4);
    const quint32 checksum = checksumCrc32C(
        reinterpret_cast<quint8 *>(bytes.data()), uint(bytes.size()));
    bytes[bytes.size() - 4] = char(checksum & 0xff);
    bytes[bytes.size() - 3] = char((checksum >> 8) & 0xff);
    bytes[bytes.size() - 2] = char((checksum >> 16) & 0xff);
    bytes[bytes.size() - 1] = char((checksum >> 24) & 0xff);
}

} // namespace

class CompatibilityTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void ethernetIpv4UdpGolden();
    void ethernetIpv4TcpGolden();
    void vlanIpv4UdpGolden();
    void ethernetIpv6UdpGolden();
    void userScriptHostApiAndByteConversion();
    void userScriptHostMethods();
    void userScriptHostChecksumCompatibility();
    void userScriptRuntimeException();
    void userScriptDeferredRuntimeException();
    void userScriptDeferredFrameSizeException();
    void userScriptDeferredProtocolIdException();
    void userScriptDeferredChecksumException();
    void nativeStreamsRoundTripAndValidation();
    void nativeSessionRoundTrip();
    void nativeUserScriptsRoundTrip();
    void versionCompatibility();
    void rpcLoopbackHandshake();
#ifdef OSTINATO_QTFREE_DRONE_PATH
    void qtClientToQtFreeDrone();
#endif
    void rpcHeaderGolden();
    void rpcIncrementalAndMultipleFrames();
    void rpcRejectsInvalidInput();
};

void CompatibilityTests::initTestCase()
{
    QCoreApplication::setApplicationName("Ostinato compatibility tests");
    QCoreApplication::instance()->setProperty("version", "test");
    QCoreApplication::instance()->setProperty("revision", "test");
    OstProtocolManager = new ProtocolManager;
}

void CompatibilityTests::cleanupTestCase()
{
    delete OstProtocolManager;
    OstProtocolManager = nullptr;
}

void CompatibilityTests::ethernetIpv4UdpGolden()
{
    const QByteArray actual = packetBytes(false);
    QCOMPARE(actual.toHex(), QByteArray(
        "00112233445566778899aabb08004500002e1234000040117c54c0000201"
        "c633640230390035001ad1fa010203040102030401020304010203040102"));
    QCOMPARE(actual.mid(24, 2).toHex(), QByteArray("7c54"));
    QCOMPARE(actual.mid(40, 2).toHex(), QByteArray("d1fa"));
}

void CompatibilityTests::ethernetIpv4TcpGolden()
{
    const QByteArray actual = packetBytes(true);
    QCOMPARE(actual.toHex(), QByteArray(
        "00112233445566778899aabb08004500002e1234000040067c5fc0000201"
        "c6336402303901bb0102030405060708501810006c7f0000010203040102"));
    QCOMPARE(actual.mid(24, 2).toHex(), QByteArray("7c5f"));
    QCOMPARE(actual.mid(50, 2).toHex(), QByteArray("6c7f"));
}

void CompatibilityTests::vlanIpv4UdpGolden()
{
    const QByteArray actual = packetBytes(false, true);
    QCOMPARE(actual.toHex(), QByteArray(
        "00112233445566778899aabb8100a02a08004500002a1234000040117c58"
        "c0000201c6336402303900350016d6080102030401020304010203040102"));
    QCOMPARE(actual.mid(28, 2).toHex(), QByteArray("7c58"));
    QCOMPARE(actual.mid(44, 2).toHex(), QByteArray("d608"));
}

void CompatibilityTests::ethernetIpv6UdpGolden()
{
    const QByteArray actual = ipv6UdpPacketBytes();
    QCOMPARE(actual.toHex(), QByteArray(
        "00112233445566778899aabb86dd600000000018114020010db800000000"
        "000000000000000120010db8000000000000000000000002303900350018"
        "63c301020304010203040102030401020304"));
    QCOMPARE(actual.mid(18, 2).toHex(), QByteArray("0018"));
    QCOMPARE(actual.mid(60, 2).toHex(), QByteArray("63c3"));
}

void CompatibilityTests::userScriptHostApiAndByteConversion()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.name = 'Baseline';\n"
        "protocol.protocolFrameSizeVariable = true;\n"
        "protocol.protocolFrameVariableCount = 3;\n"
        "protocol.protocolFrameValue = function(i) {\n"
        "  return [i, 255, 256, -1, 1.9, '2'];\n"
        "};\n"
        "protocol.protocolFrameSize = function(i) { return 6 + i; };\n"
        "protocol.protocolId = function(type) {\n"
        "  return type == Protocol.ProtocolIdEth ? 0x88b5 : 0;\n"
        "};"));

    QVERIFY(script);
    QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
    QCOMPARE(script->name(), QString("Baseline:{UserScript} [EXPERIMENTAL]"));
    QVERIFY(script->isProtocolFrameSizeVariable());
    QCOMPARE(script->protocolFrameVariableCount(), 3);
    QCOMPARE(script->protocolFrameSize(2), 8);
    QCOMPARE(script->protocolId(AbstractProtocol::ProtocolIdEth),
             quint32(0x88b5));
    QCOMPARE(script->fieldData(UserScriptProtocol::userScript_program,
                               AbstractProtocol::FieldFrameValue, 2)
                 .toByteArray().toHex(),
             QByteArray("02ff00ff0102"));
}

void CompatibilityTests::userScriptHostMethods()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.reset();\n"
        "protocol.setProtocolFrameSizeVariable(true);\n"
        "protocol.setProtocolFrameVariableCount(5);\n"
        "var variable = protocol.isProtocolFrameSizeVariable();\n"
        "protocol.protocolFrameValue = function(i) {\n"
        "  return [protocol.protocolFrameOffset(i), variable ? 1 : 0];\n"
        "};\n"
        "protocol.protocolFrameSize = function(i) { return 2; };"));

    QVERIFY(script);
    QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
    QCOMPARE(script->name(), QString(":{UserScript} [EXPERIMENTAL]"));
    QVERIFY(script->isProtocolFrameSizeVariable());
    QCOMPARE(script->protocolFrameVariableCount(), 5);
    QCOMPARE(script->fieldData(UserScriptProtocol::userScript_program,
                               AbstractProtocol::FieldFrameValue)
                 .toByteArray().toHex(),
             QByteArray("0001"));
}

void CompatibilityTests::userScriptHostChecksumCompatibility()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.protocolFrameValue = function(i) {\n"
        "  var header = typeof protocol.protocolFrameHeaderCksum === 'function';\n"
        "  var payload = protocol.protocolFramePayloadCksum();\n"
        "  return [protocol.payloadProtocolId(Protocol.ProtocolIdEth),\n"
        "    protocol.protocolFrameOffset(), protocol.protocolFramePayloadSize(),\n"
        "    protocol.isProtocolFramePayloadValueVariable() ? 1 : 0,\n"
        "    protocol.isProtocolFramePayloadSizeVariable() ? 1 : 0,\n"
        "    protocol.protocolFramePayloadVariableCount(),\n"
        "    0, header ? 1 : 0, payload >> 8, payload,\n"
        "    Protocol.CksumIp, Protocol.CksumIpPseudo, Protocol.CksumTcpUdp,\n"
        "    Protocol.IncludeCksumField];\n"
        "};\n"
        "protocol.protocolFrameSize = function(i) { return 14; };\n"
        "protocol.protocolFrameCksum = function(i, type, flags) {\n"
        "  if (i === undefined) return 0x1234;\n"
        "  if (i === 2 && type === Protocol.CksumIp &&\n"
        "      flags === Protocol.IncludeCksumField) return;\n"
        "  return 0xabcd;\n"
        "};"));

    QVERIFY(script);
    QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
    QCOMPARE(script->fieldData(UserScriptProtocol::userScript_program,
                               AbstractProtocol::FieldFrameValue)
                 .toByteArray().toHex(),
             QByteArray("00002e0000000001ffff00010201"));
    QCOMPARE(script->protocolFrameCksum(
                 2, AbstractProtocol::CksumIp,
                 AbstractProtocol::IncludeCksumField),
             quint32(0));
    QVERIFY(script->isScriptValid());
}

void CompatibilityTests::userScriptRuntimeException()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.protocolFrameValue = function(i) {\n"
        "  throw new Error('baseline failure');\n"
        "};\n"
        "protocol.protocolFrameSize = function(i) { return 0; };"));

    QVERIFY(script);
    QVERIFY(!script->isScriptValid());
    QCOMPARE(script->userScriptErrorLineNumber(), 2);
    QCOMPARE(script->userScriptErrorText(), QString("Error: baseline failure"));
    QCOMPARE(script->protocolFrameSize(), 0);
    QCOMPARE(script->fieldData(UserScriptProtocol::userScript_program,
                               AbstractProtocol::FieldFrameValue)
                 .toByteArray(),
             QByteArray());
}

void CompatibilityTests::userScriptDeferredRuntimeException()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.protocolFrameValue = function(i) {\n"
        "  if (i === undefined) return [0];\n"
        "  if (i === 7) throw new Error('deferred failure');\n"
        "  return [i];\n"
        "};\n"
        "protocol.protocolFrameSize = function(i) { return 1; };"));

    QVERIFY(script);
    QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
    QCOMPARE(script->fieldData(UserScriptProtocol::userScript_program,
                               AbstractProtocol::FieldFrameValue, 3)
                 .toByteArray().toHex(),
             QByteArray("03"));
    QCOMPARE(script->fieldData(UserScriptProtocol::userScript_program,
                               AbstractProtocol::FieldFrameValue, 7)
                 .toByteArray(),
             QByteArray());
    QVERIFY(script->isScriptValid());
    QCOMPARE(script->userScriptErrorLineNumber(), 6);
    QCOMPARE(script->userScriptErrorText(), QString());
}

void CompatibilityTests::userScriptDeferredFrameSizeException()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.protocolFrameValue = function() { return [0]; };\n"
        "protocol.protocolFrameSize = function(i) {\n"
        "  if (i === 7) throw 7;\n"
        "  return i === undefined ? 1 : i + 10;\n"
        "};"));

    QVERIFY(script);
    QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
    QCOMPARE(script->protocolFrameSize(7), 7);
    QCOMPARE(script->protocolFrameSize(2), 12);
    QVERIFY(script->isScriptValid());
    QCOMPARE(script->userScriptErrorLineNumber(), 5);
    QCOMPARE(script->userScriptErrorText(), QString());
}

void CompatibilityTests::userScriptDeferredProtocolIdException()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.protocolFrameValue = function() { return [0]; };\n"
        "protocol.protocolFrameSize = function() { return 1; };\n"
        "protocol.protocolId = function(type) {\n"
        "  if (type === undefined) return 0;\n"
        "  if (type === Protocol.ProtocolIdEth) throw new Error('id failure');\n"
        "  if (type === Protocol.ProtocolIdIp) throw 7;\n"
        "  if (type === Protocol.ProtocolIdLlc) throw { valueOf: function() { throw new Error('conversion failure'); } };\n"
        "  return 0x88b5;\n"
        "};"));

    QVERIFY(script);
    QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
    QCOMPARE(script->protocolId(AbstractProtocol::ProtocolIdEth), quint32(0));
    QCOMPARE(script->protocolId(AbstractProtocol::ProtocolIdIp), quint32(7));
    QCOMPARE(script->protocolId(AbstractProtocol::ProtocolIdLlc), quint32(0));
    QCOMPARE(script->protocolId(AbstractProtocol::ProtocolIdTcpUdp),
             quint32(0x88b5));
    QVERIFY(script->isScriptValid());
    QCOMPARE(script->userScriptErrorLineNumber(), 9);
    QCOMPARE(script->userScriptErrorText(), QString());
}

void CompatibilityTests::userScriptDeferredChecksumException()
{
    StreamBase stream;
    UserScriptProtocol *script = loadUserScript(stream, QString::fromLatin1(
        "protocol.protocolFrameValue = function() { return [0, 0]; };\n"
        "protocol.protocolFrameSize = function() { return 2; };\n"
        "protocol.protocolFrameCksum = function(i, type, flags) {\n"
        "  if (i === undefined) return 0x1234;\n"
        "  if (i === 7) throw 7;\n"
        "  return 0xabcd;\n"
        "};"));

    QVERIFY(script);
    QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
    QCOMPARE(script->protocolFrameCksum(
                 7, AbstractProtocol::CksumIp,
                 AbstractProtocol::IncludeCksumField),
             quint32(7));
    QCOMPARE(script->protocolFrameCksum(
                 2, AbstractProtocol::CksumIp,
                 AbstractProtocol::IncludeCksumField),
             quint32(0xabcd));
    QVERIFY(script->isScriptValid());
    QCOMPARE(script->userScriptErrorLineNumber(), 7);
    QCOMPARE(script->userScriptErrorText(), QString());
}

void CompatibilityTests::nativeStreamsRoundTripAndValidation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString validPath = dir.filePath("streams.ostm");
    QString error;
    OstProto::StreamConfigList original;
    original.mutable_port_id()->set_id(7);
    OstProto::Stream *stream = original.add_stream();
    stream->mutable_stream_id()->set_id(11);
    stream->mutable_core()->set_name("baseline");

    QVERIFY2(fileFormat.save(original, validPath, error), qPrintable(error));
    QByteArray valid = readBytes(validPath);
    QVERIFY(valid.startsWith(QByteArray::fromHex(
        "120aa7b74f5354494e41544f")));
    QCOMPARE(quint8(valid.at(valid.size() - 5)), quint8(0x7d));
    QByteArray zeroed = valid;
    const quint32 stored = quint8(valid.at(valid.size() - 4))
        | (quint32(quint8(valid.at(valid.size() - 3))) << 8)
        | (quint32(quint8(valid.at(valid.size() - 2))) << 16)
        | (quint32(quint8(valid.at(valid.size() - 1))) << 24);
    std::memset(zeroed.data() + zeroed.size() - 4, 0, 4);
    QCOMPARE(stored, checksumCrc32C(
        reinterpret_cast<quint8 *>(zeroed.data()), uint(zeroed.size())));
    QCOMPARE(stored, quint32(0xe2d726e0));

    OstProto::StreamConfigList restored;
    QVERIFY2(fileFormat.open(validPath, restored, error), qPrintable(error));
    QCOMPARE(restored.SerializeAsString(), original.SerializeAsString());

    QByteArray badMagic = valid;
    badMagic[2] = char(badMagic.at(2) ^ 1);
    const QString badMagicPath = dir.filePath("bad-magic.ostm");
    QVERIFY(writeBytes(badMagicPath, badMagic));
    error.clear();
    QVERIFY(!fileFormat.open(badMagicPath, restored, error));
    QVERIFY(error.contains("magic", Qt::CaseInsensitive));

    QByteArray badCrc = valid;
    badCrc[badCrc.size() - 1] = char(badCrc.at(badCrc.size() - 1) ^ 1);
    const QString badCrcPath = dir.filePath("bad-crc.ostm");
    QVERIFY(writeBytes(badCrcPath, badCrc));
    error.clear();
    QVERIFY(!fileFormat.open(badCrcPath, restored, error));
    QVERIFY(error.contains("checksum", Qt::CaseInsensitive));

    QByteArray badVersion = valid;
    const QByteArray versionFields = QByteArray::fromHex("0801100018022004");
    const int versionOffset = badVersion.indexOf(versionFields);
    QVERIFY(versionOffset >= 0);
    badVersion[versionOffset + 3] = 1;
    refreshFileChecksum(badVersion);
    const QString badVersionPath = dir.filePath("bad-version.ostm");
    QVERIFY(writeBytes(badVersionPath, badVersion));
    error.clear();
    QVERIFY(!fileFormat.open(badVersionPath, restored, error));
    QVERIFY(error.contains("incompatible", Qt::CaseInsensitive));
}

void CompatibilityTests::nativeSessionRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("session.ossn");
    QString error;
    OstProto::SessionContent original;
    OstProto::PortGroupContent *group = original.add_port_groups();
    group->set_server_name("127.0.0.1");
    group->set_server_port(7878);

    QVERIFY2(ossnFileFormat.save(original, path, error), qPrintable(error));
    OstProto::SessionContent restored;
    QVERIFY2(ossnFileFormat.open(path, restored, error), qPrintable(error));
    QCOMPARE(restored.SerializeAsString(), original.SerializeAsString());
}

void CompatibilityTests::nativeUserScriptsRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString scripts[] = {
        QString::fromLatin1(
            "protocol.name='VLAN/MPLS field';\n"
            "protocol.protocolFrameValue=function(i){return [0x81,0,0,1];};\n"
            "protocol.protocolFrameSize=function(i){if(i===7)throw 7; return 4;};\n"
            "protocol.protocolId=function(type){if(type===undefined)return 0; if(type===Protocol.ProtocolIdEth)throw new Error('deferred'); return 0x8100;};\n"
            "protocol.protocolFrameCksum=function(i){if(i===undefined)return 0x1234; if(i===2)return; if(i===7)throw 7; if(i===8)throw new Error('checksum'); return 0xabcd;};"),
        QString::fromLatin1(
            "protocol.name='Telemetry shim';\n"
            "protocol.protocolFrameValue=function(i){return [0xde,0xad,0xbe,0xef];};\n"
            "protocol.protocolFrameSize=function(i){if(i===7)throw 7; return 4;};\n"
            "protocol.protocolId=function(type){if(type===undefined)return 0; if(type===Protocol.ProtocolIdEth)throw new Error('deferred'); return 0x88b5;};\n"
            "protocol.protocolFrameCksum=function(i){if(i===undefined)return 0x1234; if(i===2)return; if(i===7)throw 7; if(i===8)throw new Error('checksum'); return 0xabcd;};")
    };

    for (int format = 0; format < 2; ++format) {
        OstProto::Stream source;
        source.mutable_stream_id()->set_id(format + 20);
        source.mutable_core()->set_frame_len(64);
        OstProto::UserScript *userScript = addProtocol(
            source, OstProto::Protocol::kUserScriptFieldNumber)
                ->MutableExtension(OstProto::userScript);
        userScript->set_program(scripts[format].toStdString());
        addProtocol(source, OstProto::Protocol::kPayloadFieldNumber)
            ->MutableExtension(OstProto::payload)->set_pattern(0x12345678);

        QString error;
        OstProto::Stream restored;
        if (format == 0) {
            OstProto::StreamConfigList list;
            list.mutable_port_id()->set_id(7);
            *list.add_stream() = source;
            const QString path = dir.filePath("script.ostm");
            QVERIFY2(fileFormat.save(list, path, error), qPrintable(error));
            OstProto::StreamConfigList opened;
            QVERIFY2(fileFormat.open(path, opened, error), qPrintable(error));
            restored = opened.stream(0);
        } else {
            OstProto::SessionContent session;
            *session.add_port_groups()->add_ports()->add_streams() = source;
            const QString path = dir.filePath("script.ossn");
            QVERIFY2(ossnFileFormat.save(session, path, error), qPrintable(error));
            OstProto::SessionContent opened;
            QVERIFY2(ossnFileFormat.open(path, opened, error), qPrintable(error));
            restored = opened.port_groups(0).ports(0).streams(0);
        }

        QCOMPARE(restored.protocol(0).GetExtension(OstProto::userScript).program(),
                 scripts[format].toStdString());
        StreamBase stream;
        stream.protoDataCopyFrom(restored);
        UserScriptProtocol *script = userScriptProtocol(stream);
        QVERIFY(script);
        QVERIFY2(script->isScriptValid(), qPrintable(script->userScriptErrorText()));
        QCOMPARE(script->protocolFrameCksum(
                     2, AbstractProtocol::CksumIp,
                     AbstractProtocol::IncludeCksumField),
                 quint32(0)); // callable returned undefined, no fallback
        QCOMPARE(script->protocolFrameSize(7), 7);
        QCOMPARE(script->protocolId(AbstractProtocol::ProtocolIdEth), quint32(0));
        QCOMPARE(script->protocolFrameCksum(
                     7, AbstractProtocol::CksumIp,
                     AbstractProtocol::IncludeCksumField),
                 quint32(7));
        QCOMPARE(script->protocolFrameCksum(
                     8, AbstractProtocol::CksumIp,
                     AbstractProtocol::IncludeCksumField),
                 quint32(0));
        QCOMPARE(script->protocolFrameCksum(
                     3, AbstractProtocol::CksumIp,
                     AbstractProtocol::IncludeCksumField),
                 quint32(0xabcd)); // exceptions don't contaminate calls
        QVERIFY(script->isScriptValid());
    }
}

void CompatibilityTests::versionCompatibility()
{
    QCOMPARE(checkVersionCompatibility("1.2.99", "1.2.0"), VersionCompatible);
    QCOMPARE(checkVersionCompatibility("2.2", "1.2.0"), VersionIncompatible);
    QCOMPARE(checkVersionCompatibility("1.3", "1.2.0"), VersionIncompatible);
    QCOMPARE(checkVersionCompatibility("1", "1.2.0"), VersionInvalid);
}

void CompatibilityTests::rpcLoopbackHandshake()
{
    LoopbackService service;
    RpcServer server(false);
    QVERIFY(server.registerService(&service, QHostAddress::LocalHost, 0));

    OstProto::Notification notification;
    {
        PbRpcChannel gatedChannel(
            "127.0.0.1", server.serverPort(), notification);
        OstProto::OstService::Stub gatedStub(&gatedChannel);
        QSignalSpy connected(&gatedChannel, SIGNAL(connected()));
        QSignalSpy disconnected(&gatedChannel, SIGNAL(disconnected()));
        gatedChannel.establish();
        QTRY_COMPARE(connected.count(), 1);

        OstProto::Void *request = new OstProto::Void;
        OstProto::PortIdList *response = new OstProto::PortIdList;
        PbRpcController *controller = new PbRpcController(request, response);
        bool done = false;
        TestClosure closure(&done);
        gatedStub.getPortIdList(controller, request, response, &closure);
        QTRY_VERIFY(done);
        QVERIFY(controller->Failed());
        QCOMPARE(controller->ErrorString(),
                 QString("version compatibility check pending"));
        QCOMPARE(response->port_id_size(), 0);
        QTRY_COMPARE(disconnected.count(), 1);
        delete controller;
    }

    PbRpcChannel channel("127.0.0.1", server.serverPort(), notification);
    OstProto::OstService::Stub stub(&channel);
    QSignalSpy connected(&channel, SIGNAL(connected()));
    QSignalSpy disconnected(&channel, SIGNAL(disconnected()));
    channel.establish();
    QTRY_COMPARE(connected.count(), 1);

    OstProto::VersionInfo *versionRequest = new OstProto::VersionInfo;
    OstProto::VersionCompatibility *versionResponse =
        new OstProto::VersionCompatibility;
    versionRequest->set_version("1.2.123");
    PbRpcController *versionController = new PbRpcController(
        versionRequest, versionResponse);
    bool versionDone = false;
    TestClosure versionClosure(&versionDone);
    QCOMPARE(service.GetDescriptor()->FindMethodByName("checkVersion")->index(), 15);
    stub.checkVersion(versionController, versionRequest, versionResponse,
                      &versionClosure);
    QTRY_VERIFY(versionDone);
    QVERIFY(!versionController->Failed());
    QCOMPARE(versionResponse->result(),
             OstProto::VersionCompatibility::kCompatible);

    OstProto::Void *listRequest = new OstProto::Void;
    OstProto::PortIdList *listResponse = new OstProto::PortIdList;
    PbRpcController *listController = new PbRpcController(
        listRequest, listResponse);
    bool listDone = false;
    TestClosure listClosure(&listDone);
    stub.getPortIdList(listController, listRequest, listResponse, &listClosure);
    QTRY_VERIFY(listDone);
    QVERIFY(!listController->Failed());
    QCOMPARE(listResponse->port_id_size(), 1);
    QCOMPARE(listResponse->port_id(0).id(), quint32(42));

    delete listController;
    delete versionController;
    channel.tearDown();
    QTRY_COMPARE(disconnected.count(), 1);
}

#ifdef OSTINATO_QTFREE_DRONE_PATH
void CompatibilityTests::qtClientToQtFreeDrone()
{
    QTcpServer portAllocator;
    QVERIFY(portAllocator.listen(QHostAddress::LocalHost, 0));
    const quint16 port = portAllocator.serverPort();
    portAllocator.close();

    QProcess droneProcess;
    droneProcess.setProcessChannelMode(QProcess::MergedChannels);
    droneProcess.start(OSTINATO_QTFREE_DRONE_PATH,
                       {"-d", "-p", QString::number(port)});
    QVERIFY2(droneProcess.waitForStarted(), qPrintable(droneProcess.errorString()));

    bool listening = false;
    for (int attempt = 0; attempt < 100 && !listening; ++attempt) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, port);
        listening = probe.waitForConnected(100);
        if (!listening)
            QTest::qWait(50);
    }
    QVERIFY2(listening, droneProcess.readAll().constData());

    OstProto::Notification notification;
    PbRpcChannel channel("127.0.0.1", port, notification);
    OstProto::OstService::Stub stub(&channel);
    QSignalSpy connected(&channel, SIGNAL(connected()));
    channel.establish();
    QTRY_COMPARE(connected.count(), 1);

    auto *versionRequest = new OstProto::VersionInfo;
    auto *versionResponse = new OstProto::VersionCompatibility;
    versionRequest->set_version("1.4.99");
    versionRequest->set_client_name("qt-compatibility-test");
    auto *versionController = new PbRpcController(versionRequest, versionResponse);
    bool versionDone = false;
    TestClosure versionClosure(&versionDone);
    stub.checkVersion(versionController, versionRequest, versionResponse, &versionClosure);
    QTRY_VERIFY(versionDone);
    QVERIFY2(!versionController->Failed(), qPrintable(versionController->ErrorString()));
    QCOMPARE(versionResponse->result(), OstProto::VersionCompatibility::kCompatible);

    auto *request = new OstProto::Void;
    auto *response = new OstProto::PortIdList;
    auto *controller = new PbRpcController(request, response);
    bool done = false;
    TestClosure closure(&done);
    stub.getPortIdList(controller, request, response, &closure);
    QTRY_VERIFY(done);
    QVERIFY2(!controller->Failed(), qPrintable(controller->ErrorString()));

    QVERIFY(response->port_id_size() > 0);
    const quint32 portId = response->port_id(0).id();

    auto *statsRequest = new OstProto::PortIdList;
    statsRequest->add_port_id()->set_id(portId);
    auto *statsResponse = new OstProto::PortStatsList;
    auto *statsController = new PbRpcController(statsRequest, statsResponse);
    bool statsDone = false;
    TestClosure statsClosure(&statsDone);
    stub.getStats(statsController, statsRequest, statsResponse, &statsClosure);
    QTRY_VERIFY(statsDone);
    QVERIFY2(!statsController->Failed(), qPrintable(statsController->ErrorString()));
    QCOMPARE(statsResponse->port_stats_size(), 1);

    auto *devicesRequest = new OstProto::PortId;
    devicesRequest->set_id(portId);
    auto *devicesResponse = new OstProto::PortDeviceList;
    auto *devicesController = new PbRpcController(devicesRequest, devicesResponse);
    bool devicesDone = false;
    TestClosure devicesClosure(&devicesDone);
    stub.getDeviceList(devicesController, devicesRequest, devicesResponse, &devicesClosure);
    QTRY_VERIFY(devicesDone);
    QVERIFY2(!devicesController->Failed(), qPrintable(devicesController->ErrorString()));

    auto *startCaptureRequest = new OstProto::PortIdList;
    startCaptureRequest->add_port_id()->set_id(portId);
    auto *startCaptureResponse = new OstProto::Ack;
    auto *startCaptureController = new PbRpcController(
        startCaptureRequest, startCaptureResponse);
    bool startCaptureDone = false;
    TestClosure startCaptureClosure(&startCaptureDone);
    stub.startCapture(startCaptureController, startCaptureRequest,
                      startCaptureResponse, &startCaptureClosure);
    QTRY_VERIFY(startCaptureDone);
    QVERIFY2(!startCaptureController->Failed(),
             qPrintable(startCaptureController->ErrorString()));
    QCOMPARE(startCaptureResponse->status(), OstProto::Ack::kRpcSuccess);

    auto *stopCaptureRequest = new OstProto::PortIdList;
    stopCaptureRequest->add_port_id()->set_id(portId);
    auto *stopCaptureResponse = new OstProto::Ack;
    auto *stopCaptureController = new PbRpcController(
        stopCaptureRequest, stopCaptureResponse);
    bool stopCaptureDone = false;
    TestClosure stopCaptureClosure(&stopCaptureDone);
    stub.stopCapture(stopCaptureController, stopCaptureRequest,
                     stopCaptureResponse, &stopCaptureClosure);
    QTRY_VERIFY(stopCaptureDone);
    QVERIFY2(!stopCaptureController->Failed(),
             qPrintable(stopCaptureController->ErrorString()));
    QCOMPARE(stopCaptureResponse->status(), OstProto::Ack::kRpcSuccess);

    auto *bufferRequest = new OstProto::PortId;
    bufferRequest->set_id(portId);
    auto *bufferResponse = new OstProto::CaptureBuffer;
    auto *bufferController = new PbRpcController(bufferRequest, bufferResponse);
    QBuffer captureBuffer;
    QVERIFY(captureBuffer.open(QIODevice::WriteOnly));
    bufferController->setBinaryBlob(&captureBuffer);
    bool bufferDone = false;
    TestClosure bufferClosure(&bufferDone);
    stub.getCaptureBuffer(bufferController, bufferRequest, bufferResponse,
                          &bufferClosure);
    QTRY_VERIFY(bufferDone);
    QVERIFY2(!bufferController->Failed(),
             qPrintable(bufferController->ErrorString()));
    QVERIFY(captureBuffer.data().size() >= 24); // pcap global header

    delete bufferController;
    delete stopCaptureController;
    delete startCaptureController;
    delete devicesController;
    delete statsController;
    delete controller;
    delete versionController;
    channel.tearDown();
    droneProcess.terminate();
    QVERIFY2(droneProcess.waitForFinished(10000), droneProcess.readAll().constData());
    QCOMPARE(droneProcess.exitStatus(), QProcess::NormalExit);
}
#endif

void CompatibilityTests::rpcHeaderGolden()
{
    const QByteArray frame = rpcFrame(PB_MSG_TYPE_REQUEST, 0x1234, "abc");
    QCOMPARE(frame.toHex(), QByteArray("0001123400000003616263"));
    PbRpcHeader header;
    QVERIFY(pbRpcDecodeHeader(reinterpret_cast<const uchar *>(frame.constData()),
                              frame.size(), header));
    QCOMPARE(header.type, quint16(PB_MSG_TYPE_REQUEST));
    QCOMPARE(header.method, quint16(0x1234));
    QCOMPARE(header.length, quint32(3));
}

void CompatibilityTests::rpcIncrementalAndMultipleFrames()
{
    const QByteArray first = rpcFrame(PB_MSG_TYPE_RESPONSE, 7, "hello");
    const QByteArray second = rpcFrame(PB_MSG_TYPE_NOTIFY, 9, "xy");
    QByteArray received;
    PbRpcHeader header;
    QByteArray payload;
    int frameSize = -1;

    received.append(first.left(3));
    QCOMPARE(pbRpcDecodeFrame(received, 0, header, payload, frameSize),
             PbRpcFrameIncomplete);
    received.append(first.mid(3, 5));
    QCOMPARE(pbRpcDecodeFrame(received, 0, header, payload, frameSize),
             PbRpcFrameIncomplete);
    received.append(first.mid(8, 2));
    QCOMPARE(pbRpcDecodeFrame(received, 0, header, payload, frameSize),
             PbRpcFrameIncomplete);
    received.append(first.mid(10));
    received.append(second);

    QCOMPARE(pbRpcDecodeFrame(received, 0, header, payload, frameSize),
             PbRpcFrameComplete);
    QCOMPARE(header.type, quint16(PB_MSG_TYPE_RESPONSE));
    QCOMPARE(payload, QByteArray("hello"));
    QCOMPARE(frameSize, first.size());
    QCOMPARE(pbRpcDecodeFrame(received, frameSize, header, payload, frameSize),
             PbRpcFrameComplete);
    QCOMPARE(header.type, quint16(PB_MSG_TYPE_NOTIFY));
    QCOMPARE(payload, QByteArray("xy"));
}

void CompatibilityTests::rpcRejectsInvalidInput()
{
    const QByteArray invalid = rpcFrame(99, 1, QByteArray());
    PbRpcHeader header;
    QByteArray payload;
    int frameSize = -1;
    QCOMPARE(pbRpcDecodeFrame(invalid, 0, header, payload, frameSize),
             PbRpcFrameInvalid);
    QCOMPARE(pbRpcDecodeFrame(invalid, -1, header, payload, frameSize),
             PbRpcFrameInvalid);

    QByteArray oversized(PB_HDR_SIZE, 0);
    pbRpcEncodeHeader(oversized.data(), PB_MSG_TYPE_REQUEST, 1,
                      quint32(INT_MAX));
    QCOMPARE(pbRpcDecodeFrame(oversized, 0, header, payload, frameSize),
             PbRpcFrameInvalid);

    pbRpcEncodeHeader(oversized.data(), PB_MSG_TYPE_REQUEST, 1,
                      quint32(INT_MAX - PB_HDR_SIZE));
    QCOMPARE(pbRpcDecodeFrame(oversized, 0, header, payload, frameSize),
             PbRpcFrameIncomplete);
}

QTEST_GUILESS_MAIN(CompatibilityTests)

#include "compatibility_tests.moc"
