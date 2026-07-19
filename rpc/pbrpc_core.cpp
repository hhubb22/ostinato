#include "pbrpc_core.h"

#include <limits>
#include <stdexcept>

namespace pbrpc {

bool isValidMessageType(std::uint16_t type)
{
    return type >= static_cast<std::uint16_t>(MessageType::Request)
        && type <= static_cast<std::uint16_t>(MessageType::Notification);
}

std::vector<std::uint8_t> encodeHeader(MessageType type, std::uint16_t method,
                                       std::uint32_t length)
{
    const std::uint16_t rawType = static_cast<std::uint16_t>(type);
    return {static_cast<std::uint8_t>(rawType >> 8),
            static_cast<std::uint8_t>(rawType),
            static_cast<std::uint8_t>(method >> 8),
            static_cast<std::uint8_t>(method),
            static_cast<std::uint8_t>(length >> 24),
            static_cast<std::uint8_t>(length >> 16),
            static_cast<std::uint8_t>(length >> 8),
            static_cast<std::uint8_t>(length)};
}

bool decodeHeader(ByteView bytes, Header &header)
{
    if (bytes.size < kHeaderSize || bytes.data == nullptr)
        return false;
    const std::uint16_t type = (std::uint16_t(bytes.data[0]) << 8) | bytes.data[1];
    header.type = static_cast<MessageType>(type);
    header.method = (std::uint16_t(bytes.data[2]) << 8) | bytes.data[3];
    header.length = (std::uint32_t(bytes.data[4]) << 24)
                  | (std::uint32_t(bytes.data[5]) << 16)
                  | (std::uint32_t(bytes.data[6]) << 8) | bytes.data[7];
    return true;
}

std::vector<std::uint8_t> encodeFrame(MessageType type, std::uint16_t method,
                                      ByteView payload)
{
    if (payload.size > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("RPC payload is too large");
    if (payload.size && payload.data == nullptr)
        throw std::invalid_argument("null RPC payload");
    auto result = encodeHeader(type, method, static_cast<std::uint32_t>(payload.size));
    if (payload.size)
        result.insert(result.end(), payload.data, payload.data + payload.size);
    return result;
}

FrameParser::FrameParser(std::uint32_t maxPayload) : maxPayload_(maxPayload) {}

bool FrameParser::push(ByteView bytes)
{
    if (failed_)
        return false;
    if (bytes.size && bytes.data == nullptr) {
        failed_ = true;
        error_ = "null input";
        return false;
    }
    if (bytes.size)
        buffer_.insert(buffer_.end(), bytes.data, bytes.data + bytes.size);
    parse();
    return !failed_;
}

void FrameParser::parse()
{
    std::size_t consumed = 0;
    while (buffer_.size() - consumed >= kHeaderSize) {
        Header header;
        decodeHeader(ByteView(buffer_.data() + consumed, kHeaderSize), header);
        if (!isValidMessageType(static_cast<std::uint16_t>(header.type))) {
            failed_ = true;
            error_ = "invalid RPC message type";
            break;
        }
        if (header.length > maxPayload_) {
            failed_ = true;
            error_ = "RPC payload exceeds configured maximum";
            break;
        }
        const std::size_t frameSize = kHeaderSize + std::size_t(header.length);
        if (buffer_.size() - consumed < frameSize)
            break;
        Frame frame;
        frame.header = header;
        frame.payload.assign(buffer_.begin() + consumed + kHeaderSize,
                             buffer_.begin() + consumed + frameSize);
        frames_.push_back(std::move(frame));
        consumed += frameSize;
    }
    if (consumed)
        buffer_.erase(buffer_.begin(), buffer_.begin() + consumed);
}

Frame FrameParser::popFrame()
{
    if (frames_.empty())
        throw std::out_of_range("no RPC frame available");
    Frame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

void FrameParser::reset()
{
    buffer_.clear();
    frames_.clear();
    failed_ = false;
    error_.clear();
}

} // namespace pbrpc
