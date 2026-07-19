#include "pbrpc_core.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

pbrpc::ByteView view(const std::vector<std::uint8_t> &bytes)
{
    return {bytes.data(), bytes.size()};
}

std::vector<std::uint8_t> frame(pbrpc::MessageType type, std::uint16_t method,
                                const std::string &payload)
{
    return pbrpc::encodeFrame(type, method, {payload.data(), payload.size()});
}
}

int main()
{
    using pbrpc::MessageType;
    const auto golden = pbrpc::encodeHeader(MessageType::Request, 0x1234, 0x01020304);
    check(golden == std::vector<std::uint8_t>({0, 1, 0x12, 0x34, 1, 2, 3, 4}),
          "golden big-endian header");

    const auto response = frame(MessageType::Response, 7, "partial");
    pbrpc::FrameParser parser;
    for (std::size_t i = 0; i < response.size(); ++i) {
        check(parser.push({response.data() + i, 1}), "byte-at-a-time input accepted");
        check(parser.frameCount() == (i + 1 == response.size() ? 1U : 0U),
              "partial frame not emitted early");
    }
    auto parsed = parser.popFrame();
    check(parsed.header.type == MessageType::Response && parsed.header.method == 7,
          "partial frame header");
    check(std::string(parsed.payload.begin(), parsed.payload.end()) == "partial",
          "partial frame payload");

    std::vector<std::uint8_t> concatenated;
    const std::vector<MessageType> types = {MessageType::Request, MessageType::Response,
        MessageType::BinaryBlob, MessageType::Error, MessageType::Notification};
    for (std::size_t i = 0; i < types.size(); ++i) {
        auto item = frame(types[i], static_cast<std::uint16_t>(i), "payload" + std::to_string(i));
        concatenated.insert(concatenated.end(), item.begin(), item.end());
    }
    check(parser.push(view(concatenated)) && parser.frameCount() == types.size(),
          "concatenated request/response/blob/error/notification parsed");
    for (std::size_t i = 0; i < types.size(); ++i) {
        parsed = parser.popFrame();
        check(parsed.header.type == types[i] && parsed.header.method == i,
              "frames emitted FIFO");
        check(std::string(parsed.payload.begin(), parsed.payload.end())
                  == "payload" + std::to_string(i),
              "all message type payloads preserved");
    }

    pbrpc::FrameParser invalid;
    const std::vector<std::uint8_t> invalidType = {0, 6, 0, 0, 0, 0, 0, 0};
    check(!invalid.push(view(invalidType)) && invalid.failed(), "invalid type rejected");
    invalid.reset();
    const auto oversized = pbrpc::encodeHeader(MessageType::Request, 0, 5);
    pbrpc::FrameParser limited(4);
    check(!limited.push(view(oversized)) && limited.failed(), "oversize rejected at header");

    std::cout << "pbrpc_core tests passed\n";
    return 0;
}
