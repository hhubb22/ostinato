#ifndef OSTINATO_CONTROLLER_STDIO_PROTOCOL_H
#define OSTINATO_CONTROLLER_STDIO_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ostinato { namespace controller {

constexpr std::uint32_t kMaxFrameSize = 1024 * 1024;

enum class FrameType : std::uint8_t {
    Json = 1,
    Binary = 2
};

struct Frame {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> payload;
};

class FrameDecoder {
public:
    explicit FrameDecoder(std::uint32_t maxFrameSize = kMaxFrameSize)
        : maxFrameSize_(maxFrameSize) {}

    bool feed(const std::uint8_t *data, std::size_t size,
              std::vector<Frame> &frames, std::string &error);

private:
    const std::uint32_t maxFrameSize_;
    std::vector<std::uint8_t> buffer_;
};

std::vector<std::uint8_t> encodeFrame(
    FrameType type, const std::uint8_t *payload, std::size_t size);
std::vector<std::uint8_t> encodeFrame(FrameType type, const std::string &payload);

} } // namespace ostinato::controller

#endif
