#pragma once

#include <Core/Types.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>

#include <functional>
#include <optional>
#include <vector>

namespace DB
{

/** A substring index over the dictionary of a text index: an FM-index of the sorted tokens of one part.
  *
  * Text. The tokens are concatenated as `SEP t0 SEP t1 ... SEP t(d-1) TERM`, where `TERM` = 0x00 and `SEP` = 0x01
  * are the two smallest byte values and may not occur in a token. Every substring of a token is then a prefix of
  * some suffix of this text, and a substring that would cross a token boundary has to contain `SEP`.
  *
  * Search. Sorting all suffixes gives the suffix array; the suffixes starting with a needle form one contiguous range
  * of rows in it. The index does not store the suffix array. It stores the Burrows-Wheeler transform (BWT): for each
  * row, the byte just before that suffix. With the table `C` (how many bytes of the text are smaller than a byte) and
  * `rank` (how often a byte occurs in the BWT before a row), the range is found by backward search, one step per
  * byte of the needle from its last byte to its first. The size of the range is the number of occurrences, known
  * before any dictionary block is read.
  *
  * Locate. A row is mapped to a dictionary block without a sampled suffix array. Row 0 is the suffix `TERM`; rows
  * 1..d are the suffixes that start with `SEP`, and since the tokens are sorted and distinct they are in token order,
  * so row i + 1 is the separator before token i and its block is i / `tokens_per_block`. The LF mapping moves from a
  * row to the row of the suffix that starts one byte earlier, so a match that starts at byte k of a token (counting
  * from 1) reaches that token's separator row after k steps. Tokens longer than `HYBRID_STRIDE` bytes have a sample
  * every `HYBRID_STRIDE` bytes, stored as its row and block, so that no walk takes more than `HYBRID_STRIDE` steps.
  *
  * On disk: a format version, three scalars, the BWT, and the sample rows with their blocks. `C`, the alphabet and
  * the occurrence counts are rebuilt from the BWT on load. The loader checks the shape of the sample table (sorted,
  * in range) but not each block value, which would cost a walk per sample; a damaged value is left to the part
  * checksums, as is a damaged BWT.
  */
class TextIndexSubstringIndex
{
public:
    static constexpr UInt8 SEPARATOR = 0x01;
    static constexpr UInt8 TERMINATOR = 0x00;
    /// Inside a token longer than this, every `HYBRID_STRIDE`-th byte is a sample, so a locate walk takes at most
    /// this many steps.
    static constexpr UInt32 HYBRID_STRIDE = 64;

    /// Builds the index over `num_tokens` tokens in dictionary order, distinct; `get_token(i)` returns the i-th token.
    /// Returns nothing when a token contains the separator or the terminator byte, or the text is longer than
    /// `max_text_size`.
    static std::optional<TextIndexSubstringIndex> build(
        size_t num_tokens, const std::function<std::string_view(size_t)> & get_token, size_t tokens_per_block, size_t max_text_size);

    void serialize(WriteBuffer & out) const;
    static TextIndexSubstringIndex deserialize(ReadBuffer & in);

    /// The half-open range of suffix-array rows that start with `needle`, or nothing if there are none. Its size is the
    /// number of occurrences in the dictionary, so a token that contains the needle twice counts twice.
    std::optional<std::pair<UInt32, UInt32>> search(std::string_view needle) const;

    /// The dictionary blocks holding the tokens of the suffix range, sorted and without duplicates.
    std::vector<size_t> blocksOfRange(UInt32 begin, UInt32 end) const;

    size_t numBlocks() const { return num_blocks; }
    size_t memoryUsageBytes() const;

private:
    /// Byte values that do not occur in the text get this symbol.
    static constexpr UInt16 ABSENT = 0xFFFF;

    void buildDerivedTables();
    UInt32 rank(UInt16 symbol, UInt32 position) const;
    UInt32 lf(UInt32 position) const;
    UInt32 locateBlock(UInt32 position) const;

    UInt32 text_size = 0;
    UInt32 occurrence_block = 512;
    UInt32 tokens_per_block = 0;

    /// Persisted: the BWT, byte `i` is the byte before the suffix in row `i` (the last column of the sorted rotations).
    std::string bwt;
    /// Persisted: the rows of the samples inside long tokens, increasing, and the block of each, for `locateBlock`.
    std::vector<UInt32> hybrid_rows;
    std::vector<UInt32> hybrid_blocks;

    /// Derived: the number of tokens (separators in the BWT) and of dictionary blocks.
    UInt32 num_tokens = 0;
    UInt32 num_blocks = 0;
    /// Derived: `C[c]` is the number of bytes in the text smaller than `c`, which is the first row whose suffix starts
    /// with `c`; 257 entries, so `C[c + 1]` ends that range.
    std::vector<UInt32> C;
    /// Derived: only the byte values that occur get a symbol, which keeps `occurrences` small for a small alphabet.
    std::vector<UInt16> symbol_of_byte;
    std::vector<UInt8> byte_of_symbol;
    /// Derived: the count of every symbol before each multiple of `occurrence_block`; `rank` adds the rest by scanning
    /// at most `occurrence_block` bytes of the BWT.
    std::vector<UInt32> occurrences;
};

}
