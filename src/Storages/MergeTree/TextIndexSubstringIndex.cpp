#include <Storages/MergeTree/TextIndexSubstringIndex.h>

#include <Common/Exception.h>
#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>
#include <IO/WriteHelpers.h>

#include <libsais.h>

#include <algorithm>
#include <limits>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}

namespace
{

constexpr UInt64 FORMAT_VERSION = 2;

bool hasStructuralByte(std::string_view s)
{
    return s.find(static_cast<char>(TextIndexSubstringIndex::TERMINATOR)) != std::string_view::npos
        || s.find(static_cast<char>(TextIndexSubstringIndex::SEPARATOR)) != std::string_view::npos;
}

/// Suffix array by induced sorting (libsais), linear time; needs 4 bytes per text byte on top of the text.
std::vector<Int32> buildSuffixArray(const std::string & text)
{
    std::vector<Int32> suffix_array(text.size());
    const int rc = libsais(reinterpret_cast<const uint8_t *>(text.data()), suffix_array.data(), static_cast<Int32>(text.size()), 0, nullptr);
    if (rc != 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "libsais failed with code {} on a text of {} bytes", rc, text.size());
    return suffix_array;
}

template <typename T>
void writeVector(const std::vector<T> & v, WriteBuffer & out)
{
    writeVarUInt(v.size(), out);
    out.write(reinterpret_cast<const char *>(v.data()), v.size() * sizeof(T));
}

/// The size comes from the file, so the data is read in 1 MiB steps: a damaged size fails on the missing data instead
/// of allocating everything it claims first.
template <typename Container>
void readInSteps(Container & v, ReadBuffer & in, size_t size)
{
    using T = typename Container::value_type;
    constexpr size_t step = (1 << 20) / sizeof(T);
    v.clear();
    while (v.size() < size)
    {
        const size_t old_size = v.size();
        const size_t n = std::min(step, size - old_size);
        v.resize(old_size + n);
        in.readStrict(reinterpret_cast<char *>(v.data() + old_size), n * sizeof(T));
    }
}

template <typename T>
void readVector(std::vector<T> & v, ReadBuffer & in, size_t max_size)
{
    UInt64 size = 0;
    readVarUInt(size, in);
    if (size > max_size)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: a vector of {} elements, at most {} expected", size, max_size);
    readInSteps(v, in, size);
}

UInt32 readUInt32(ReadBuffer & in, const char * what)
{
    UInt64 value = 0;
    readVarUInt(value, in);
    if (value > std::numeric_limits<UInt32>::max())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: {} is {}", what, value);
    return static_cast<UInt32>(value);
}

}

std::optional<TextIndexSubstringIndex> TextIndexSubstringIndex::build(
    size_t num_tokens_, const std::function<std::string_view(size_t)> & get_token, size_t tokens_per_block_, size_t max_text_size)
{
    TextIndexSubstringIndex index;
    index.tokens_per_block = static_cast<UInt32>(tokens_per_block_);

    std::string text;
    /// Text offsets of the samples inside long tokens, with the block of the token, in text order.
    std::vector<std::pair<UInt32, UInt32>> samples;
    for (size_t i = 0; i < num_tokens_; ++i)
    {
        const auto token = get_token(i);
        /// A token containing `SEP` or `TERM` would break the token order of the separator rows, so such a part gets
        /// no index.
        if (hasStructuralByte(token))
            return {};
        const size_t boundary = text.size();
        text += static_cast<char>(SEPARATOR);
        text += token;
        /// libsais uses signed 32-bit offsets, and the cap bounds build memory; above either the part gets no index and
        /// patterns use the dictionary scan.
        if (text.size() > max_text_size || text.size() >= static_cast<size_t>(std::numeric_limits<Int32>::max()))
            return {};
        /// Offset `boundary + m` is byte m - 1 of the token, so samples sit at token bytes 63, 127, ...
        if (token.size() > HYBRID_STRIDE)
            for (size_t m = HYBRID_STRIDE; m <= token.size(); m += HYBRID_STRIDE)
                samples.emplace_back(static_cast<UInt32>(boundary + m), static_cast<UInt32>(i / tokens_per_block_));
    }
    text += static_cast<char>(TERMINATOR);
    index.text_size = static_cast<UInt32>(text.size());

    auto suffix_array = buildSuffixArray(text);

    /// The BWT byte of a row is the byte before its suffix; the suffix at offset 0 wraps around to `TERM`.
    index.bwt.resize(index.text_size);
    for (UInt32 i = 0; i < index.text_size; ++i)
    {
        const UInt32 offset = static_cast<UInt32>(suffix_array[i]);
        index.bwt[i] = text[(offset + index.text_size - 1) % index.text_size];
    }

    /// Walking the suffix array in row order emits the sample rows increasing, as `locateBlock` needs. A bit per text
    /// byte marks the samples, and the block comes from a binary search in `samples`, which is in text order.
    if (!samples.empty())
    {
        std::vector<bool> is_sample(index.text_size, false);
        for (const auto & [offset, block] : samples)
            is_sample[offset] = true;
        for (UInt32 row = 0; row < index.text_size; ++row)
        {
            const UInt32 offset = static_cast<UInt32>(suffix_array[row]);
            if (!is_sample[offset])
                continue;
            auto it = std::lower_bound(samples.begin(), samples.end(), std::make_pair(offset, 0u));
            index.hybrid_rows.push_back(row);
            index.hybrid_blocks.push_back(it->second);
        }
    }

    index.buildDerivedTables();
    return index;
}

void TextIndexSubstringIndex::buildDerivedTables()
{
    std::vector<UInt32> counts(256, 0);
    for (unsigned char c : bwt)
        ++counts[c];
    num_tokens = counts[SEPARATOR];
    num_blocks = tokens_per_block ? static_cast<UInt32>((static_cast<UInt64>(num_tokens) + tokens_per_block - 1) / tokens_per_block) : 0;

    symbol_of_byte.assign(256, ABSENT);
    byte_of_symbol.clear();
    for (size_t c = 0; c < 256; ++c)
    {
        if (counts[c] == 0)
            continue;
        symbol_of_byte[c] = static_cast<UInt16>(byte_of_symbol.size());
        byte_of_symbol.push_back(static_cast<UInt8>(c));
    }

    C.assign(257, 0);
    for (size_t c = 0; c < 256; ++c)
        C[c + 1] = C[c] + counts[c];

    const size_t num_symbols = byte_of_symbol.size();
    const size_t num_occurrence_blocks = text_size / occurrence_block + 1;
    occurrences.assign(num_occurrence_blocks * num_symbols, 0);
    std::vector<UInt32> running(num_symbols, 0);
    /// One checkpoint per `occurrence_block` positions, including position `text_size`, which `rank` at the end of the
    /// range needs.
    for (UInt32 i = 0; i <= text_size; ++i)
    {
        if (i % occurrence_block == 0)
            std::copy(running.begin(), running.end(), occurrences.begin() + (i / occurrence_block) * num_symbols);
        if (i < text_size)
            ++running[symbol_of_byte[static_cast<UInt8>(bwt[i])]];
    }
}

void TextIndexSubstringIndex::serialize(WriteBuffer & out) const
{
    writeVarUInt(FORMAT_VERSION, out);
    writeVarUInt(text_size, out);
    writeVarUInt(occurrence_block, out);
    writeVarUInt(tokens_per_block, out);
    writeStringBinary(bwt, out);
    writeVector(hybrid_rows, out);
    writeVector(hybrid_blocks, out);
}

TextIndexSubstringIndex TextIndexSubstringIndex::deserialize(ReadBuffer & in)
{
    UInt64 format_version = 0;
    readVarUInt(format_version, in);
    if (format_version != FORMAT_VERSION)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Unknown format version {} of the substring index of a text index, expected {}; rebuild the index with `ALTER TABLE ... MATERIALIZE INDEX`",
            format_version, FORMAT_VERSION);

    TextIndexSubstringIndex index;
    index.text_size = readUInt32(in, "text size");
    index.occurrence_block = readUInt32(in, "occurrence block");
    index.tokens_per_block = readUInt32(in, "tokens per block");
    if (index.text_size == 0 || index.occurrence_block == 0 || index.tokens_per_block == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: a zero parameter");

    UInt64 bwt_size = 0;
    readVarUInt(bwt_size, in);
    if (bwt_size != index.text_size)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: BWT of {} bytes for a text of {}", bwt_size, index.text_size);
    readInSteps(index.bwt, in, index.text_size);

    /// There is at most one sample per `HYBRID_STRIDE` bytes of text.
    readVector(index.hybrid_rows, in, index.text_size / HYBRID_STRIDE);
    readVector(index.hybrid_blocks, in, index.text_size / HYBRID_STRIDE);

    /// The index decides which blocks are read, so a damaged index can silently drop matches. The checks below are the
    /// ones that cost no walk: one terminator, matching sample tables, sample rows increasing and above the separator
    /// rows, and blocks in range.
    index.buildDerivedTables();

    if (index.C[TERMINATOR + 1] != 1)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: {} terminators in the BWT", index.C[TERMINATOR + 1]);

    if (index.hybrid_rows.size() != index.hybrid_blocks.size())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: {} sample rows for {} sample blocks", index.hybrid_rows.size(), index.hybrid_blocks.size());

    for (size_t i = 0; i < index.hybrid_rows.size(); ++i)
    {
        /// A sample suffix starts inside a token, so its row is after row 0 (`TERM`) and rows 1..d (`SEP`).
        const UInt32 row = index.hybrid_rows[i];
        if (row <= index.num_tokens || row >= index.text_size || (i > 0 && row <= index.hybrid_rows[i - 1]) || index.hybrid_blocks[i] >= index.num_blocks)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: sample row {} with block {} out of order or out of range", row, index.hybrid_blocks[i]);
    }

    return index;
}

/// The number of occurrences of `symbol` in `bwt[0, position)`: the checkpoint of its block plus a scan of the rest.
UInt32 TextIndexSubstringIndex::rank(UInt16 symbol, UInt32 position) const
{
    const UInt32 block = position / occurrence_block;
    UInt32 result = occurrences[static_cast<size_t>(block) * byte_of_symbol.size() + symbol];
    const char byte = static_cast<char>(byte_of_symbol[symbol]);
    for (UInt32 i = block * occurrence_block; i < position; ++i)
        result += bwt[i] == byte;
    return result;
}

std::optional<std::pair<UInt32, UInt32>> TextIndexSubstringIndex::search(std::string_view needle) const
{
    /// No token contains `SEP` or `TERM`, so such a needle matches nothing; searching it could match across tokens.
    if (needle.empty() || hasStructuralByte(needle))
        return {};

    /// Backward search: after the step for byte k, [begin, end) holds the rows whose suffix starts with needle[k..].
    /// Prepending byte c keeps the rows whose BWT byte is c and maps them into the rows starting with c, from `C[c]`.
    UInt32 begin = 0;
    UInt32 end = text_size;
    for (size_t k = needle.size(); k-- > 0;)
    {
        const UInt8 byte = needle[k];
        const UInt16 symbol = symbol_of_byte[byte];
        if (symbol == ABSENT)
            return {};
        begin = C[byte] + rank(symbol, begin);
        end = C[byte] + rank(symbol, end);
        if (begin >= end)
            return {};
    }
    return std::make_pair(begin, end);
}

/// LF mapping: the row of the suffix that starts one byte before the suffix in row `position`.
UInt32 TextIndexSubstringIndex::lf(UInt32 position) const
{
    const UInt8 byte = bwt[position];
    return C[byte] + rank(symbol_of_byte[byte], position);
}

UInt32 TextIndexSubstringIndex::locateBlock(UInt32 row) const
{
    /// Each LF step moves the suffix one byte back in its token. The token's separator row, or a sample of a long
    /// token, is reached within `HYBRID_STRIDE` steps, so a longer walk means the index is damaged.
    for (UInt32 steps = 0;; ++steps)
    {
        /// Rows 1..d are the separators in token order, so row r is the separator of token r - 1.
        if (row != 0 && row <= num_tokens)
            return (row - 1) / tokens_per_block;

        auto it = std::lower_bound(hybrid_rows.begin(), hybrid_rows.end(), row);
        if (it != hybrid_rows.end() && *it == row)
            return hybrid_blocks[it - hybrid_rows.begin()];

        if (steps == HYBRID_STRIDE)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index is corrupted: no token boundary within {} steps", HYBRID_STRIDE);
        row = lf(row);
    }
}

std::vector<size_t> TextIndexSubstringIndex::blocksOfRange(UInt32 begin, UInt32 end) const
{
    std::vector<UInt8> selected(num_blocks, 0);
    for (UInt32 row = begin; row < end; ++row)
    {
        const UInt32 block = locateBlock(row);
        if (block >= num_blocks)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Substring index of the text index names block {} of {}", block, num_blocks);
        selected[block] = 1;
    }

    std::vector<size_t> blocks;
    for (size_t block = 0; block < selected.size(); ++block)
        if (selected[block])
            blocks.push_back(block);
    return blocks;
}

size_t TextIndexSubstringIndex::memoryUsageBytes() const
{
    return bwt.size() + C.size() * sizeof(UInt32) + symbol_of_byte.size() * sizeof(UInt16) + byte_of_symbol.size()
        + occurrences.size() * sizeof(UInt32) + (hybrid_rows.size() + hybrid_blocks.size()) * sizeof(UInt32);
}

}
