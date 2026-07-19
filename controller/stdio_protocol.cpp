#include "stdio_protocol.h"

#include <algorithm>
#include <limits>

namespace ostinato { namespace controller {
namespace {

std::uint32_t readBigEndian(const std::uint8_t *value)
{
    return (static_cast<std::uint32_t>(value[0]) << 24) |
           (static_cast<std::uint32_t>(value[1]) << 16) |
           (static_cast<std::uint32_t>(value[2]) << 8) |
           static_cast<std::uint32_t>(value[3]);
}

void writeBigEndian(std::uint8_t *output, std::uint32_t value)
{
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

} // namespace

bool FrameDecoder::feed(const std::uint8_t *data, std::size_t size,
                        std::vector<Frame> &frames, std::string &error)
{
    if (size && !data) {
        error = "null frame input";
        return false;
    }
    buffer_.insert(buffer_.end(), data, data + size);

    std::size_t consumed = 0;
    while (buffer_.size() - consumed >= 4) {
        const std::uint32_t frameSize = readBigEndian(buffer_.data() + consumed);
        if (frameSize < 1) {
            error = "frame body must contain a type byte";
            return false;
        }
        if (frameSize > maxFrameSize_) {
            error = "frame exceeds configured maximum";
            return false;
        }
        if (buffer_.size() - consumed - 4 < frameSize)
            break;

        Frame frame;
        frame.type = buffer_[consumed + 4];
        const auto payloadBegin = buffer_.begin() +
            static_cast<std::ptrdiff_t>(consumed + 5);
        const auto payloadEnd = buffer_.begin() +
            static_cast<std::ptrdiff_t>(consumed + 4 + frameSize);
        frame.payload.assign(payloadBegin, payloadEnd);
        frames.push_back(std::move(frame));
        consumed += 4 + frameSize;
    }

    if (consumed)
        buffer_.erase(buffer_.begin(), buffer_.begin() +
            static_cast<std::ptrdiff_t>(consumed));

    // The caller reads stdin in small fixed chunks. Once a valid header has
    // been checked, an incomplete buffer can never need more than max + header.
    if (buffer_.size() > static_cast<std::size_t>(maxFrameSize_) + 4) {
        error = "frame buffer exceeds configured maximum";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encodeFrame(
    FrameType type, const std::uint8_t *payload, std::size_t size)
{
    if (size > static_cast<std::size_t>(kMaxFrameSize - 1) ||
            size > std::numeric_limits<std::uint32_t>::max() - 1)
        return {};
    std::vector<std::uint8_t> result(5 + size);
    writeBigEndian(result.data(), static_cast<std::uint32_t>(size + 1));
    result[4] = static_cast<std::uint8_t>(type);
    if (size)
        std::copy(payload, payload + size, result.begin() + 5);
    return result;
}

std::vector<std::uint8_t> encodeFrame(FrameType type, const std::string &payload)
{
    return encodeFrame(type,
        reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
}

} } // namespace ostinato::controller
