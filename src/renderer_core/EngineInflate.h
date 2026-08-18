#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::compress {

// DEFLATE, RFC 1951, and its zlib wrapper, RFC 1950.
//
// Written rather than linked because the installed material archive stores
// every entry as a zlib stream and nothing else in this renderer reads one.
// A partial decompressor is not an option: the archive's entries use dynamic
// Huffman blocks, so the stored and fixed paths alone would decode almost
// nothing, and a decoder that silently produces the wrong bytes on a block
// shape it does not handle is worse than one that refuses.
enum class InflateError : std::uint8_t
{
    None,
    // The stream ended inside a block that promised more.
    TruncatedInput,
    // A block type the format does not define, or a stored block whose length
    // and complement disagree.
    InvalidBlock,
    // A code that no entry of the current alphabet reaches, a distance
    // pointing before the start of the output, or a length symbol outside the
    // table.
    InvalidCode,
    // A code-length table that does not describe a complete prefix code.
    // Accepting one would decode later symbols to whatever fell out.
    IncompleteCodeLengths,
    // zlib only: the wrapper's compression method or its header check.
    InvalidWrapper,
    // zlib only: the trailing Adler-32 does not match what was decoded.
    ChecksumMismatch,
    // The output grew past what any caller can be expecting, which is what a
    // malformed stream looks like when it decodes to a run that never ends.
    OutputTooLarge,
};

// Raw DEFLATE. `output` is cleared first and grown as the stream decodes.
[[nodiscard]] InflateError Inflate(
    std::span<const std::byte> input,
    std::vector<std::byte>& output) noexcept;

// The same stream inside its zlib wrapper, whose Adler-32 is checked.
[[nodiscard]] InflateError InflateZlib(
    std::span<const std::byte> input,
    std::vector<std::byte>& output) noexcept;

[[nodiscard]] std::uint32_t Adler32(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] const char* ToString(InflateError error) noexcept;

}  // namespace vf::renderer::compress
