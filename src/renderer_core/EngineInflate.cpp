#include "renderer_core/EngineInflate.h"

#include <array>
#include <vector>

namespace vf::renderer::compress {
namespace {

// A malformed stream can describe a run that never ends. The cap is far above
// any material file in the archive -- the largest is a few tens of kilobytes
// -- and exists so a bad stream fails instead of exhausting memory.
constexpr std::size_t kMaximumOutput = 64u * 1024u * 1024u;

constexpr std::uint32_t kAdlerModulus = 65521u;

// RFC 1951 section 3.2.5. Indexed by symbol - 257.
constexpr std::array<std::uint16_t, 29> kLengthBase{
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase{
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra{
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13};
// The order the code-length alphabet declares its own lengths in, chosen so
// the rarely used ones fall at the end and can be omitted entirely.
constexpr std::array<std::uint8_t, 19> kCodeLengthOrder{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr std::size_t kMaximumCodeBits = 15;

// Canonical Huffman decoding by the counting method of RFC 1951 section
// 3.2.2: symbols sorted by code length, with a first-code per length. This
// needs no tree and no table wider than the alphabet, and it decodes a bit at
// a time -- fast enough here, where the whole archive is under two megabytes.
struct HuffmanTable
{
    std::array<std::uint16_t, kMaximumCodeBits + 1> countOfLength{};
    std::vector<std::uint16_t> symbols;
};

[[nodiscard]] InflateError BuildHuffman(
    const std::span<const std::uint8_t> lengths,
    HuffmanTable& table)
{
    table.countOfLength.fill(0);
    for (const auto length : lengths) {
        if (length > kMaximumCodeBits) return InflateError::InvalidCode;
        ++table.countOfLength[length];
    }
    // Length zero means the symbol has no code, not a code of no bits.
    table.countOfLength[0] = 0;

    // A prefix code is complete when the Kraft sum is exactly one. Over-
    // subscribed means two symbols share a code, which is rejected here
    // rather than decoded into whichever one the table happens to hold.
    auto available = 1;
    for (std::size_t bits = 1; bits <= kMaximumCodeBits; ++bits) {
        available <<= 1;
        available -= table.countOfLength[bits];
        if (available < 0) return InflateError::IncompleteCodeLengths;
    }

    std::array<std::uint16_t, kMaximumCodeBits + 1> offsets{};
    std::uint16_t running = 0;
    for (std::size_t bits = 1; bits <= kMaximumCodeBits; ++bits) {
        offsets[bits] = running;
        running = static_cast<std::uint16_t>(
            running + table.countOfLength[bits]);
    }
    table.symbols.assign(running, 0);
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        const auto length = lengths[symbol];
        if (length == 0) continue;
        table.symbols[offsets[length]++] = static_cast<std::uint16_t>(symbol);
    }
    // An incomplete code is legal in exactly one case: a block with no
    // matches needs no distances, and zlib emits a one-symbol distance code
    // rather than an empty one. Anything else incomplete would let some bit
    // patterns decode to nothing.
    if (available > 0 && running > 1) {
        return InflateError::IncompleteCodeLengths;
    }
    return InflateError::None;
}

class BitReader
{
public:
    explicit BitReader(const std::span<const std::byte> input) noexcept
        : input_{input}
    {
    }

    // Least significant bit first, which is how DEFLATE packs everything
    // except the Huffman codes themselves.
    [[nodiscard]] bool Read(const std::size_t count, std::uint32_t& value)
        noexcept
    {
        value = 0;
        for (std::size_t bit = 0; bit < count; ++bit) {
            std::uint32_t single = 0;
            if (!ReadBit(single)) return false;
            value |= single << bit;
        }
        return true;
    }

    [[nodiscard]] bool ReadBit(std::uint32_t& value) noexcept
    {
        if (position_ >= input_.size()) return false;
        value = (static_cast<std::uint32_t>(input_[position_]) >> bitOffset_) &
            1u;
        if (++bitOffset_ == 8) {
            bitOffset_ = 0;
            ++position_;
        }
        return true;
    }

    void AlignToByte() noexcept
    {
        if (bitOffset_ != 0) {
            bitOffset_ = 0;
            ++position_;
        }
    }

    [[nodiscard]] bool ReadBytes(
        const std::size_t count,
        std::span<const std::byte>& bytes) noexcept
    {
        if (input_.size() - position_ < count) return false;
        bytes = input_.subspan(position_, count);
        position_ += count;
        return true;
    }

    // Huffman codes are packed most significant bit first, so a code is
    // accumulated by shifting left as bits arrive -- the opposite of every
    // other field in this format, and the classic place to get it wrong.
    [[nodiscard]] InflateError Decode(
        const HuffmanTable& table,
        std::uint16_t& symbol) noexcept
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (std::size_t bits = 1; bits <= kMaximumCodeBits; ++bits) {
            std::uint32_t single = 0;
            if (!ReadBit(single)) return InflateError::TruncatedInput;
            code |= static_cast<int>(single);
            const int count = table.countOfLength[bits];
            if (code - first < count) {
                symbol = table.symbols[
                    static_cast<std::size_t>(index + (code - first))];
                return InflateError::None;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return InflateError::InvalidCode;
    }

private:
    std::span<const std::byte> input_;
    std::size_t position_{};
    std::size_t bitOffset_{};
};

[[nodiscard]] InflateError DecodeBlock(
    BitReader& reader,
    const HuffmanTable& literals,
    const HuffmanTable& distances,
    std::vector<std::byte>& output)
{
    for (;;) {
        std::uint16_t symbol = 0;
        const auto decoded = reader.Decode(literals, symbol);
        if (decoded != InflateError::None) return decoded;
        if (symbol < 256) {
            if (output.size() >= kMaximumOutput) {
                return InflateError::OutputTooLarge;
            }
            output.push_back(static_cast<std::byte>(symbol));
            continue;
        }
        if (symbol == 256) return InflateError::None;
        const auto lengthIndex = static_cast<std::size_t>(symbol - 257);
        if (lengthIndex >= kLengthBase.size()) {
            return InflateError::InvalidCode;
        }
        std::uint32_t extra = 0;
        if (!reader.Read(kLengthExtra[lengthIndex], extra)) {
            return InflateError::TruncatedInput;
        }
        const auto length =
            static_cast<std::size_t>(kLengthBase[lengthIndex]) + extra;

        std::uint16_t distanceSymbol = 0;
        const auto distanceDecoded = reader.Decode(distances, distanceSymbol);
        if (distanceDecoded != InflateError::None) return distanceDecoded;
        if (distanceSymbol >= kDistanceBase.size()) {
            return InflateError::InvalidCode;
        }
        std::uint32_t distanceExtra = 0;
        if (!reader.Read(kDistanceExtra[distanceSymbol], distanceExtra)) {
            return InflateError::TruncatedInput;
        }
        const auto distance =
            static_cast<std::size_t>(kDistanceBase[distanceSymbol]) +
            distanceExtra;
        // A distance reaching before the start of the output is the shape a
        // corrupt stream takes, and following it would read whatever happens
        // to precede the buffer.
        if (distance == 0 || distance > output.size()) {
            return InflateError::InvalidCode;
        }
        if (output.size() + length > kMaximumOutput) {
            return InflateError::OutputTooLarge;
        }
        // One byte at a time on purpose: a match may overlap its own output,
        // which is how a run of a single byte is coded, and a block copy
        // would read bytes that have not been written yet.
        const auto source = output.size() - distance;
        const std::vector<std::byte> match{output.begin() + source,
            output.begin() + source + std::min(length, distance)};
        for (std::size_t written = 0; written < length; ++written) {
            output.push_back(match[written % match.size()]);
        }
    }
}

[[nodiscard]] InflateError DecodeStored(
    BitReader& reader,
    std::vector<std::byte>& output)
{
    reader.AlignToByte();
    std::span<const std::byte> header;
    if (!reader.ReadBytes(4, header)) return InflateError::TruncatedInput;
    const auto length = static_cast<std::size_t>(header[0]) |
        (static_cast<std::size_t>(header[1]) << 8);
    const auto complement = static_cast<std::size_t>(header[2]) |
        (static_cast<std::size_t>(header[3]) << 8);
    // The only integrity check the format gives a stored block.
    if ((length ^ 0xFFFFu) != complement) return InflateError::InvalidBlock;
    std::span<const std::byte> stored;
    if (!reader.ReadBytes(length, stored)) {
        return InflateError::TruncatedInput;
    }
    if (output.size() + length > kMaximumOutput) {
        return InflateError::OutputTooLarge;
    }
    output.insert(output.end(), stored.begin(), stored.end());
    return InflateError::None;
}

// The fixed alphabets of RFC 1951 section 3.2.6, built from their lengths
// rather than written out, so they go through exactly the same decoder a
// dynamic block does.
[[nodiscard]] InflateError BuildFixedTables(
    HuffmanTable& literals,
    HuffmanTable& distances)
{
    std::array<std::uint8_t, 288> literalLengths{};
    for (std::size_t symbol = 0; symbol < 144; ++symbol) {
        literalLengths[symbol] = 8;
    }
    for (std::size_t symbol = 144; symbol < 256; ++symbol) {
        literalLengths[symbol] = 9;
    }
    for (std::size_t symbol = 256; symbol < 280; ++symbol) {
        literalLengths[symbol] = 7;
    }
    for (std::size_t symbol = 280; symbol < 288; ++symbol) {
        literalLengths[symbol] = 8;
    }
    // Thirty-two, not thirty. The fixed distance code assigns five bits to
    // every one of 32 symbols; the last two never occur in a valid stream but
    // they are part of the code, and leaving them out makes it incomplete --
    // 30/32 rather than 1 -- so every fixed block is refused.
    std::array<std::uint8_t, 32> distanceLengths{};
    distanceLengths.fill(5);
    const auto built = BuildHuffman(literalLengths, literals);
    if (built != InflateError::None) return built;
    return BuildHuffman(distanceLengths, distances);
}

[[nodiscard]] InflateError BuildDynamicTables(
    BitReader& reader,
    HuffmanTable& literals,
    HuffmanTable& distances)
{
    std::uint32_t literalCount = 0;
    std::uint32_t distanceCount = 0;
    std::uint32_t codeLengthCount = 0;
    if (!reader.Read(5, literalCount) || !reader.Read(5, distanceCount) ||
        !reader.Read(4, codeLengthCount)) {
        return InflateError::TruncatedInput;
    }
    literalCount += 257;
    distanceCount += 1;
    codeLengthCount += 4;
    if (literalCount > 288 || distanceCount > 32) {
        return InflateError::InvalidBlock;
    }

    std::array<std::uint8_t, 19> codeLengthLengths{};
    for (std::uint32_t index = 0; index < codeLengthCount; ++index) {
        std::uint32_t value = 0;
        if (!reader.Read(3, value)) return InflateError::TruncatedInput;
        codeLengthLengths[kCodeLengthOrder[index]] =
            static_cast<std::uint8_t>(value);
    }
    HuffmanTable codeLengths;
    auto built = BuildHuffman(codeLengthLengths, codeLengths);
    if (built != InflateError::None) return built;

    // The literal and distance lengths arrive as one run-length coded
    // sequence, so a repeat can carry across the boundary between them and
    // the two cannot be read separately.
    const auto total = static_cast<std::size_t>(literalCount) + distanceCount;
    std::vector<std::uint8_t> lengths;
    lengths.reserve(total);
    while (lengths.size() < total) {
        std::uint16_t symbol = 0;
        const auto decoded = reader.Decode(codeLengths, symbol);
        if (decoded != InflateError::None) return decoded;
        if (symbol < 16) {
            lengths.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        std::size_t repeat = 0;
        std::uint8_t value = 0;
        std::uint32_t extra = 0;
        if (symbol == 16) {
            // Repeats the previous length, so there has to be one.
            if (lengths.empty()) return InflateError::InvalidCode;
            if (!reader.Read(2, extra)) return InflateError::TruncatedInput;
            repeat = 3 + extra;
            value = lengths.back();
        } else if (symbol == 17) {
            if (!reader.Read(3, extra)) return InflateError::TruncatedInput;
            repeat = 3 + extra;
        } else if (symbol == 18) {
            if (!reader.Read(7, extra)) return InflateError::TruncatedInput;
            repeat = 11 + extra;
        } else {
            return InflateError::InvalidCode;
        }
        if (lengths.size() + repeat > total) return InflateError::InvalidBlock;
        lengths.insert(lengths.end(), repeat, value);
    }

    built = BuildHuffman(std::span{lengths}.first(literalCount), literals);
    if (built != InflateError::None) return built;
    return BuildHuffman(
        std::span{lengths}.subspan(literalCount, distanceCount), distances);
}

}  // namespace

std::uint32_t Adler32(const std::span<const std::byte> bytes) noexcept
{
    std::uint32_t low = 1;
    std::uint32_t high = 0;
    for (const auto value : bytes) {
        low = (low + static_cast<std::uint32_t>(value)) % kAdlerModulus;
        high = (high + low) % kAdlerModulus;
    }
    return (high << 16) | low;
}

InflateError Inflate(
    const std::span<const std::byte> input,
    std::vector<std::byte>& output) noexcept
{
    output.clear();
    try {
        BitReader reader{input};
        for (;;) {
            std::uint32_t finalBlock = 0;
            if (!reader.ReadBit(finalBlock)) {
                return InflateError::TruncatedInput;
            }
            std::uint32_t type = 0;
            if (!reader.Read(2, type)) return InflateError::TruncatedInput;

            if (type == 0) {
                const auto stored = DecodeStored(reader, output);
                if (stored != InflateError::None) return stored;
            } else if (type == 1 || type == 2) {
                HuffmanTable literals;
                HuffmanTable distances;
                const auto built = type == 1
                    ? BuildFixedTables(literals, distances)
                    : BuildDynamicTables(reader, literals, distances);
                if (built != InflateError::None) return built;
                const auto decoded =
                    DecodeBlock(reader, literals, distances, output);
                if (decoded != InflateError::None) return decoded;
            } else {
                return InflateError::InvalidBlock;
            }
            if (finalBlock != 0) return InflateError::None;
        }
    } catch (...) {
        // Only the growing containers can throw, and only by running out of
        // memory, which is the same failure the cap above exists to prevent.
        return InflateError::OutputTooLarge;
    }
}

InflateError InflateZlib(
    const std::span<const std::byte> input,
    std::vector<std::byte>& output) noexcept
{
    output.clear();
    // Two header bytes and a four byte Adler-32.
    if (input.size() < 6) return InflateError::TruncatedInput;
    const auto method = static_cast<std::uint32_t>(input[0]);
    const auto flags = static_cast<std::uint32_t>(input[1]);
    // Low nibble 8 is deflate; nothing else was ever defined.
    if ((method & 0x0Fu) != 8) return InflateError::InvalidWrapper;
    // A window this decoder does not keep. No zlib encoder emits one.
    if ((method >> 4) > 7) return InflateError::InvalidWrapper;
    if (((method << 8) | flags) % 31u != 0) {
        return InflateError::InvalidWrapper;
    }
    // A preset dictionary would have to come from the caller, and the archive
    // never uses one. Refused rather than ignored: ignoring it decodes to the
    // wrong bytes from the first match onward.
    if ((flags & 0x20u) != 0) return InflateError::InvalidWrapper;

    const auto result = Inflate(input.subspan(2, input.size() - 6), output);
    if (result != InflateError::None) return result;

    const auto trailer = input.subspan(input.size() - 4, 4);
    const auto expected = (static_cast<std::uint32_t>(trailer[0]) << 24) |
        (static_cast<std::uint32_t>(trailer[1]) << 16) |
        (static_cast<std::uint32_t>(trailer[2]) << 8) |
        static_cast<std::uint32_t>(trailer[3]);
    if (Adler32(output) != expected) return InflateError::ChecksumMismatch;
    return InflateError::None;
}

const char* ToString(const InflateError error) noexcept
{
    switch (error) {
    case InflateError::None: return "none";
    case InflateError::TruncatedInput: return "truncated input";
    case InflateError::InvalidBlock: return "invalid block";
    case InflateError::InvalidCode: return "invalid code";
    case InflateError::IncompleteCodeLengths: return "incomplete code lengths";
    case InflateError::InvalidWrapper: return "invalid wrapper";
    case InflateError::ChecksumMismatch: return "checksum mismatch";
    case InflateError::OutputTooLarge: return "output too large";
    }
    return "unknown";
}

}  // namespace vf::renderer::compress
