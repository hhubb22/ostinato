#include "client_session.h"
#include "stdio_protocol.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using google::protobuf::ListValue;
using google::protobuf::Struct;
using google::protobuf::Value;
using ostinato::client::ClientSession;
using ostinato::controller::Frame;
using ostinato::controller::FrameDecoder;
using ostinato::controller::FrameType;

constexpr auto kRpcTimeout = std::chrono::milliseconds(2000);
constexpr int kStatsIntervalMs = 500;
volatile sig_atomic_t stopped = 0;

void stopSignal(int) { stopped = 1; }

Value stringValue(const std::string &value)
{
    Value result;
    result.set_string_value(value);
    return result;
}

Value numberValue(double value)
{
    Value result;
    result.set_number_value(value);
    return result;
}

Value boolValue(bool value)
{
    Value result;
    result.set_bool_value(value);
    return result;
}

Value structValue(const Struct &value)
{
    Value result;
    result.mutable_struct_value()->CopyFrom(value);
    return result;
}

Value listValue(const ListValue &value)
{
    Value result;
    result.mutable_list_value()->CopyFrom(value);
    return result;
}

void put(Struct &value, const std::string &key, Value field)
{
    (*value.mutable_fields())[key] = std::move(field);
}

const Value *field(const Struct &value, const char *name)
{
    const auto found = value.fields().find(name);
    return found == value.fields().end() ? nullptr : &found->second;
}

bool stringField(const Struct &value, const char *name, std::string &output)
{
    const Value *candidate = field(value, name);
    if (!candidate || candidate->kind_case() != Value::kStringValue)
        return false;
    output = candidate->string_value();
    return true;
}

bool emptyArgs(const Struct &request)
{
    const Value *args = field(request, "args");
    return !args || (args->kind_case() == Value::kStructValue &&
                     args->struct_value().fields().empty());
}

bool validRequestId(const std::string &id)
{
    if (id.empty() || id.size() > 64)
        return false;
    for (unsigned char value : id)
        if (!(std::isalnum(value) || value == '-' || value == '_' ||
              value == '.' || value == ':'))
            return false;
    return true;
}

bool numericAddress(const std::string &host)
{
    in_addr ipv4 = {};
    in6_addr ipv6 = {};
    return inet_pton(AF_INET, host.c_str(), &ipv4) == 1 ||
           inet_pton(AF_INET6, host.c_str(), &ipv6) == 1;
}

std::string linkState(const OstProto::PortStats &stats)
{
    if (!stats.has_state())
        return "unknown";
    switch (stats.state().link_state()) {
    case OstProto::LinkStateUp: return "up";
    case OstProto::LinkStateDown: return "down";
    default: return "unknown";
    }
}

std::string errorCode(pbrpc::TcpRpcClient::Error error)
{
    using Error = pbrpc::TcpRpcClient::Error;
    switch (error) {
    case Error::Timeout: return "timeout";
    case Error::Canceled: return "canceled";
    case Error::Disconnected: return "disconnected";
    case Error::Transport: return "transport";
    case Error::Remote: return "remote";
    case Error::Protocol: return "protocol";
    case Error::InvalidProtobuf: return "invalid-protobuf";
    default: return "none";
    }
}

class Controller {
public:
    bool send(const Struct &value)
    {
        std::string json;
        google::protobuf::util::JsonPrintOptions options;
        options.add_whitespace = false;
        const auto status = google::protobuf::util::MessageToJsonString(
            value, &json, options);
        if (!status.ok()) {
            std::cerr << "controller: JSON serialization failed: "
                      << status.ToString() << '\n';
            return false;
        }
        const auto bytes = ostinato::controller::encodeFrame(FrameType::Json, json);
        if (bytes.empty()) {
            std::cerr << "controller: outgoing frame exceeds maximum\n";
            return false;
        }
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = ::write(STDOUT_FILENO, bytes.data() + written,
                                          bytes.size() - written);
            if (count > 0) {
                written += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            stopped = 1;
            return false;
        }
        return true;
    }

    void status(const std::string &state, const std::string &message = {})
    {
        Struct payload;
        put(payload, "state", stringValue(state));
        if (!message.empty())
            put(payload, "message", stringValue(message));
        if (!host_.empty()) {
            Struct endpoint;
            put(endpoint, "host", stringValue(host_));
            put(endpoint, "port", numberValue(port_));
            put(payload, "endpoint", structValue(endpoint));
        }
        event("status", payload);
    }

    void protocolError(const std::string &message)
    {
        Struct payload;
        put(payload, "code", stringValue("protocol"));
        put(payload, "message", stringValue(message));
        event("protocolError", payload);
    }

    bool handle(const Frame &frame)
    {
        if (frame.type != static_cast<std::uint8_t>(FrameType::Json)) {
            protocolError("unsupported frame type");
            return true;
        }
        Struct request;
        const std::string json(frame.payload.begin(), frame.payload.end());
        google::protobuf::util::JsonParseOptions options;
        options.ignore_unknown_fields = false;
        const auto parse = google::protobuf::util::JsonStringToMessage(
            json, &request, options);
        if (!parse.ok()) {
            protocolError("malformed JSON control payload");
            return true;
        }

        std::string kind, id, command;
        if (!stringField(request, "kind", kind) || kind != "request" ||
                !stringField(request, "id", id) || !validRequestId(id) ||
                !stringField(request, "command", command)) {
            protocolError("invalid request envelope");
            return true;
        }
        if (request.fields().size() > 4 ||
                (request.fields().size() == 4 && !field(request, "args"))) {
            response(id, false, "invalid-request", "unknown request field");
            return true;
        }

        if (command == "connect")
            connect(id, request);
        else if (command == "disconnect") {
            if (!emptyArgs(request))
                response(id, false, "invalid-argument", "disconnect takes no arguments");
            else {
                session_.disconnect();
                ports(false);
                status("disconnected", "Disconnected by user");
                response(id, true);
            }
        } else if (command == "reconnect") {
            if (!emptyArgs(request))
                response(id, false, "invalid-argument", "reconnect takes no arguments");
            else if (host_.empty())
                response(id, false, "invalid-state", "no previous endpoint");
            else
                reconnect(id);
        } else if (command == "shutdown") {
            if (!emptyArgs(request))
                response(id, false, "invalid-argument", "shutdown takes no arguments");
            else {
                session_.disconnect();
                response(id, true);
                stopped = 1;
            }
        } else {
            response(id, false, "unknown-command", "unknown command");
        }
        return true;
    }

    void pollStats()
    {
        if (!session_.connected())
            return;
        const auto result = session_.pollStats(kRpcTimeout);
        if (!result) {
            session_.disconnect();
            ports(false);
            status("error", errorCode(result.error) + ": " + result.message);
            return;
        }
        ports(true);
    }

private:
    void event(const std::string &name, const Struct &payload)
    {
        Struct envelope;
        put(envelope, "kind", stringValue("event"));
        put(envelope, "event", stringValue(name));
        put(envelope, "payload", structValue(payload));
        send(envelope);
    }

    void response(const std::string &id, bool ok,
                  const std::string &code = {}, const std::string &message = {})
    {
        Struct envelope;
        put(envelope, "kind", stringValue("response"));
        put(envelope, "id", stringValue(id));
        put(envelope, "ok", boolValue(ok));
        if (!ok) {
            Struct error;
            put(error, "code", stringValue(code));
            put(error, "message", stringValue(message));
            put(envelope, "error", structValue(error));
        }
        send(envelope);
    }

    void connect(const std::string &id, const Struct &request)
    {
        const Value *argsValue = field(request, "args");
        if (!argsValue || argsValue->kind_case() != Value::kStructValue) {
            response(id, false, "invalid-argument", "connect args must be an object");
            return;
        }
        const Struct &args = argsValue->struct_value();
        std::string host;
        const Value *port = field(args, "port");
        if (args.fields().size() != 2 || !stringField(args, "host", host) ||
                host.size() > 64 || !numericAddress(host) || !port ||
                port->kind_case() != Value::kNumberValue ||
                std::floor(port->number_value()) != port->number_value() ||
                port->number_value() < 1 || port->number_value() > 65535) {
            response(id, false, "invalid-argument",
                     "numeric IPv4/IPv6 host and port 1-65535 required");
            return;
        }
        host_ = host;
        port_ = static_cast<std::uint16_t>(port->number_value());
        status("connecting");
        const auto result = session_.connect(host_, port_, "1.4.0-dev", kRpcTimeout);
        finishConnection(id, result);
    }

    void reconnect(const std::string &id)
    {
        status("connecting", "Reconnecting");
        finishConnection(id, session_.reconnect(kRpcTimeout));
    }

    void finishConnection(const std::string &id, const ClientSession::Result &result)
    {
        if (!result) {
            ports(false);
            const std::string message = errorCode(result.error) + ": " + result.message;
            status("error", message);
            response(id, false, errorCode(result.error), result.message);
            return;
        }
        const auto statsResult = session_.queryStats(kRpcTimeout);
        if (!statsResult) {
            session_.disconnect();
            ports(false);
            status("error", statsResult.message);
            response(id, false, errorCode(statsResult.error), statsResult.message);
            return;
        }
        status("connected");
        ports(true);
        response(id, true);
    }

    Struct portMetadata(const ostinato::client::PortState &port) const
    {
        Struct value;
        put(value, "id", numberValue(port.id()));
        put(value, "name", stringValue(port.config().name()));
        put(value, "link", stringValue(linkState(port.stats())));
        put(value, "transmit", boolValue(port.transmitting()));
        put(value, "capture", boolValue(port.capturing()));
        put(value, "dirty", boolValue(port.dirty()));
        return value;
    }

    Struct portStats(const ostinato::client::PortState &port) const
    {
        const auto &source = port.stats();
        Struct value;
        put(value, "id", numberValue(port.id()));
        put(value, "link", stringValue(linkState(source)));
        put(value, "transmit", boolValue(port.transmitting()));
        put(value, "capture", boolValue(port.capturing()));
#define COUNTER(name) put(value, #name, stringValue(std::to_string(source.name())))
        COUNTER(rx_pkts);
        COUNTER(rx_bytes);
        COUNTER(rx_pps);
        COUNTER(rx_bps);
        COUNTER(tx_pkts);
        COUNTER(tx_bytes);
        COUNTER(tx_pps);
        COUNTER(tx_bps);
        COUNTER(rx_drops);
        COUNTER(rx_errors);
#undef COUNTER
        return value;
    }

    void ports(bool includeStats)
    {
        ListValue metadata;
        ListValue statistics;
        if (session_.connected()) {
            for (const auto &entry : session_.ports()) {
                metadata.add_values()->CopyFrom(structValue(portMetadata(entry.second)));
                if (includeStats)
                    statistics.add_values()->CopyFrom(structValue(portStats(entry.second)));
            }
        }
        Struct portsPayload;
        put(portsPayload, "ports", listValue(metadata));
        event("ports", portsPayload);

        Struct statsPayload;
        put(statsPayload, "sequence", numberValue(++sequence_));
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        put(statsPayload, "sampledAt", numberValue(static_cast<double>(now)));
        put(statsPayload, "ports", listValue(statistics));
        event("stats", statsPayload);
    }

    ClientSession session_;
    std::string host_;
    std::uint16_t port_ = 0;
    std::uint64_t sequence_ = 0;
};

} // namespace

int main()
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, stopSignal);
    signal(SIGTERM, stopSignal);
#ifdef __linux__
    const pid_t parent = getppid();
    // Graceful Electron shutdown uses the framed shutdown command. If the main
    // process itself disappears, SIGKILL guarantees a bounded RPC cannot leave
    // a temporarily orphaned controller behind.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
        std::cerr << "controller: could not install parent-death signal: "
                  << std::strerror(errno) << '\n';
    } else if (getppid() != parent) {
        return 1;
    }
#endif

    Controller controller;
    controller.status("disconnected", "Controller ready");
    FrameDecoder decoder;
    auto nextPoll = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kStatsIntervalMs);

    while (!stopped) {
        const auto now = std::chrono::steady_clock::now();
        const int timeout = static_cast<int>(std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(nextPoll - now).count()));
        pollfd input = {STDIN_FILENO, POLLIN, 0};
        const int ready = poll(&input, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            std::cerr << "controller: stdin poll failed: " << std::strerror(errno) << '\n';
            break;
        }
        if (ready > 0 && (input.revents & (POLLIN | POLLHUP))) {
            std::uint8_t bytes[16384];
            const ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count == 0)
                break;
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                std::cerr << "controller: stdin read failed: " << std::strerror(errno) << '\n';
                break;
            }
            std::vector<Frame> frames;
            std::string error;
            if (!decoder.feed(bytes, static_cast<std::size_t>(count), frames, error)) {
                controller.protocolError(error);
                break;
            }
            for (const auto &frame : frames) {
                controller.handle(frame);
                if (stopped)
                    break;
            }
        }
        if (std::chrono::steady_clock::now() >= nextPoll) {
            controller.pollStats();
            nextPoll = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(kStatsIntervalMs);
        }
    }
    return 0;
}
