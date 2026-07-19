#include "stdio_protocol.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ostinato::controller;

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void feed(FrameDecoder &decoder, const std::vector<std::uint8_t> &bytes,
          std::size_t offset, std::size_t size, std::vector<Frame> &frames)
{
    std::string error;
    require(decoder.feed(bytes.data() + offset, size, frames, error), error);
}

} // namespace

int main()
{
    try {
        const auto first = encodeFrame(FrameType::Json, std::string("{\"first\":true}"));
        const auto second = encodeFrame(FrameType::Binary,
            reinterpret_cast<const std::uint8_t *>("abc"), 3);
        require(!first.empty() && !second.empty(), "encode frames");

        FrameDecoder partial;
        std::vector<Frame> frames;
        feed(partial, first, 0, 2, frames);
        feed(partial, first, 2, 3, frames);
        require(frames.empty(), "partial frame remains buffered");
        feed(partial, first, 5, first.size() - 5, frames);
        require(frames.size() == 1 && frames[0].type == 1 &&
                    std::string(frames[0].payload.begin(), frames[0].payload.end()) ==
                        "{\"first\":true}",
                "partial frame decoded exactly");

        std::vector<std::uint8_t> coalesced(first);
        coalesced.insert(coalesced.end(), second.begin(), second.end());
        FrameDecoder joined;
        frames.clear();
        feed(joined, coalesced, 0, coalesced.size(), frames);
        require(frames.size() == 2 && frames[1].type == 2 &&
                    frames[1].payload == std::vector<std::uint8_t>({'a', 'b', 'c'}),
                "coalesced frames decoded independently");

        FrameDecoder oversized(8);
        const std::uint8_t bad[] = {0, 0, 0, 9};
        std::string error;
        frames.clear();
        require(!oversized.feed(bad, sizeof(bad), frames, error) &&
                    error.find("maximum") != std::string::npos,
                "oversized frame rejected from header without allocation");

        FrameDecoder empty;
        const std::uint8_t noType[] = {0, 0, 0, 0};
        error.clear();
        require(!empty.feed(noType, sizeof(noType), frames, error),
                "zero-length frame rejected");

        require(encodeFrame(FrameType::Json, nullptr, kMaxFrameSize).empty(),
                "outgoing maximum enforced");
        std::cout << "controller framing tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "controller framing tests failed: " << error.what() << '\n';
        return 1;
    }
}
