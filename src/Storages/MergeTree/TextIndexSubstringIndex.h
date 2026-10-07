#pragma once

#include <Core/Types.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>

#include <functional>
#include <optional>
#include <vector>

namespace DB
{

/** A substring index over the dictionary of a text index: an FM-index of the sorted tokens.
  *
  * The text is `SEP t0 SEP t1 ... SEP t(d-1) TERM`, with the separator and the terminator the two smallest bytes and
  * both excluded from tokens. The index stores the Burrows-Wheeler transform of that text; a pattern `%needle%` is
  * answered with one rank step per byte of the needle, which gives the range of suffixes starting with it, and the
  * occurrence count is known before anything is located.
  *
  * Locating uses the order of the dictionary instead of a sampled suffix array: the suffixes that start at a separator
  * are the d smallest after the terminator, and because the tokens are sorted and distinct they come in token order,
  * so the suffix-array row of the separator before token i is i + 1. Walking LF from a match inside a token reaches
  * that row after as many steps as the match is deep in the token, and the row gives the token and its dictionary
  * block. Tokens longer than `HYBRID_STRIDE` bytes carry a sample every `HYBRID_STRIDE` bytes so that the walk is
  * bounded for them too.
  *
  * On disk: the BWT, the rows of the samples inside long tokens with their blocks, and a few scalars; the counts
  * the search needs are derived from the BWT when the index is loaded. The loader checks the shape of the sample
  * table but not its values: verifying one sample costs an LF walk of up to `HYBRID_STRIDE` steps, so a wrong block
  * stored for a sample is left to the part checksums, like the BWT itself.
  */
class TextIndexSubstringIndex
{
public:
    static constexpr UInt8 SEPARATOR = 0x01;
    static constexpr UInt8 TERMINATOR = 0x00;
    /// Inside a token longer than this, every such byte is a sample; the LF walk never takes more steps than this.
    static constexpr UInt32 HYBRID_STRIDE = 64;

    /// Builds the index over `num_tokens` tokens in dictionary order, distinct; `get_token(i)` returns the i-th token.
    /// Returns nothing when the index is not built: a token holds the separator or the terminator byte, or the text
    /// is longer than `max_text_size`.
    static std::optional<TextIndexSubstringIndex> build(
        size_t num_tokens, const std::function<std::string_view(size_t)> & get_token, size_t tokens_per_block, size_t max_text_size);

    void serialize(WriteBuffer & out) const;
    static TextIndexSubstringIndex deserialize(ReadBuffer & in);

    /// The half-open range of suffixes that start with `needle`, in suffix-array order, or nothing if there are none.
    /// Its size is the number of occurrences of the needle in the dictionary, counted with multiplicity.
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

    /// Persisted: the BWT (the last column of the sorted rotations).
    std::string bwt;
    /// Persisted: the suffix-array rows of the samples inside long tokens, increasing, and the block of each.
    std::vector<UInt32> hybrid_rows;
    std::vector<UInt32> hybrid_blocks;

    /// Derived: the number of tokens (separators in the BWT) and of dictionary blocks.
    UInt32 num_tokens = 0;
    UInt32 num_blocks = 0;
    /// Derived: for each byte value, the number of smaller bytes in the text; 257 entries so that C[c + 1] ends c's bucket.
    std::vector<UInt32> C;
    /// Derived: compact alphabet, only the byte values that occur get a symbol.
    std::vector<UInt16> symbol_of_byte;
    std::vector<UInt8> byte_of_symbol;
    /// Derived: occurrence counts of every symbol before every `occurrence_block`-th position.
    std::vector<UInt32> occurrences;
};

}
