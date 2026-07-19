/* Qt-free Ostinato protobuf RPC wire framing. */
#ifndef PBRPC_CORE_H
#define PBRPC_CORE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace pbrpc {

constexpr std::size_t kHeaderSize = 8;
constexpr std::uint32_t kDefaultMaxPayload = 0x7ffffff7U;

enum class MessageType : std::uint16_t {
    Request = 1,
    Response = 2,
    BinaryBlob = 3,
    Error = 4,
    Notification = 5
};

struct ByteView {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;

    ByteView() = default;
    ByteView(const void *bytes, std::size_t length)
        : data(static_cast<const std::uint8_t *>(bytes)), size(length) {}
};

struct Header {
    MessageType type = MessageType::Request;
    std::uint16_t method = 0;
    std::uint32_t length = 0;
};

struct Frame {
    Header header;
    std::vector<std::uint8_t> payload;
};

bool isValidMessageType(std::uint16_t type);
std::vector<std::uint8_t> encodeHeader(MessageType type, std::uint16_t method,
                                       std::uint32_t length);
bool decodeHeader(ByteView bytes, Header &header);
std::vector<std::uint8_t> encodeFrame(MessageType type, std::uint16_t method,
                                      ByteView payload);

class FrameParser {
public:
    explicit FrameParser(std::uint32_t maxPayload = kDefaultMaxPayload);

    // Appends input and extracts every complete frame in wire/FIFO order.
    // Once false is returned the parser remains failed until reset().
    bool push(ByteView bytes);
    bool hasFrame() const { return !frames_.empty(); }
    std::size_t frameCount() const { return frames_.size(); }
    Frame popFrame();
    bool failed() const { return failed_; }
    const std::string &error() const { return error_; }
    void reset();

private:
    void parse();

    std::uint32_t maxPayload_;
    std::vector<std::uint8_t> buffer_;
    std::deque<Frame> frames_;
    bool failed_ = false;
    std::string error_;
};

} // namespace pbrpc

#endif
