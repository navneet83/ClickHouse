-- Tags: no-parallel-replicas
-- Tag no-parallel-replicas -- the profile events are read from the query_log of the initiator

-- A text index with `substring_index = 1` answers `LIKE '%needle%'` from an index over its dictionary instead of a
-- scan of every dictionary block. The results must be the ones of the plain scan and of no index at all.
SET enable_analyzer = 1;
SET use_query_condition_cache = 0;
SET use_text_index_like_evaluation_by_dictionary_scan = 1;
SET use_skip_indexes_on_data_read = 0;
SET query_plan_direct_read_from_text_index = 0;
SET log_queries = 1;

DROP TABLE IF EXISTS tab;
-- The argument is experimental.
CREATE TABLE tab (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id; -- { serverError SUPPORT_IS_DISABLED }
CREATE TABLE tab (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 2) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1; -- { serverError BAD_ARGUMENTS }
-- The substring index stores the dictionary block size in 32 bits.
CREATE TABLE tab (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1, dictionary_block_size = 4294967296) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1; -- { serverError BAD_ARGUMENTS }

CREATE TABLE tab (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1, dictionary_block_size = 16) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 8, allow_experimental_text_index_substring_index = 1;
-- The block counts below are per part, so the parts must stay as inserted until the explicit merge.
SYSTEM STOP MERGES tab;

-- 2000 rows, a few thousand distinct tokens; `needlefish` is in every 37th row, the hex token is unique per row.
INSERT INTO tab SELECT number, concat('w', toString(number * 7919 % 100003), ' ', if(number % 37 = 0, 'needlefish', 'other'), ' ', lower(hex(cityHash64(number))))
    FROM numbers(2000);

SELECT 'infix', count() FROM tab WHERE s LIKE '%needle%' SETTINGS log_comment = '05328 infix';
SELECT 'infix, no index', count() FROM tab WHERE s LIKE '%needle%' SETTINGS use_skip_indexes = 0;
SELECT 'infix 2', count() FROM tab WHERE s LIKE '%efish%';
SELECT 'infix 2, no index', count() FROM tab WHERE s LIKE '%efish%' SETTINGS use_skip_indexes = 0;
-- The hex tokens are unique per row, so this needle hits a handful of tokens spread over the dictionary, or none at all.
SELECT 'hex infix', count() FROM tab WHERE s LIKE '%a1b2%' SETTINGS log_comment = '05328 hex';
SELECT 'hex infix, no index', count() FROM tab WHERE s LIKE '%a1b2%' SETTINGS use_skip_indexes = 0;
SELECT 'absent', count() FROM tab WHERE s LIKE '%zzzzz%' SETTINGS log_comment = '05328 absent';
SELECT 'two patterns', count() FROM tab WHERE s LIKE '%needle%' AND s LIKE '%other%' SETTINGS log_comment = '05328 two';
SELECT 'two patterns, no index', count() FROM tab WHERE s LIKE '%needle%' AND s LIKE '%other%' SETTINGS use_skip_indexes = 0;
-- A suffix pattern is a substring search as well; a prefix pattern is a seek in the sorted dictionary and does not use the substring index.
SELECT 'suffix', count() FROM tab WHERE s LIKE '%lefish' SETTINGS log_comment = '05328 suffix';
SELECT 'suffix, no index', count() FROM tab WHERE s LIKE '%lefish' SETTINGS use_skip_indexes = 0;
SELECT 'prefix', count() FROM tab WHERE s LIKE 'needlef%' SETTINGS log_comment = '05328 prefix';
SELECT 'prefix, no index', count() FROM tab WHERE s LIKE 'needlef%' SETTINGS use_skip_indexes = 0;
SELECT 'not like', count() FROM tab WHERE s NOT LIKE '%needle%' SETTINGS log_comment = '05328 not like';
SELECT 'not like, no index', count() FROM tab WHERE s NOT LIKE '%needle%' SETTINGS use_skip_indexes = 0;
-- Direct read from the index evaluates the same predicate on the index alone.
SELECT 'direct', count() FROM tab WHERE s LIKE '%needle%' SETTINGS query_plan_direct_read_from_text_index = 1, log_comment = '05328 direct';
-- A needle in more tokens than the threshold is answered by the plain scan: `other` is in every row that is not a needlefish one.
SELECT 'common', count() FROM tab WHERE s LIKE '%othe%' SETTINGS text_index_substring_index_max_occurrences = 0, log_comment = '05328 common';
SELECT 'common, no index', count() FROM tab WHERE s LIKE '%othe%' SETTINGS use_skip_indexes = 0;
-- `match` is not evaluated through the dictionary today, so neither the scan nor the substring index is used for it.
SELECT 'regex', count() FROM tab WHERE match(s, 'needle.ish') SETTINGS log_comment = '05328 regex';
SELECT 'regex, no index', count() FROM tab WHERE match(s, 'needle.ish') SETTINGS use_skip_indexes = 0;
-- The dictionary is case-sensitive, so a case-insensitive pattern keeps the scan.
SELECT 'ilike', count() FROM tab WHERE s ILIKE '%NEEDLE%' SETTINGS log_comment = '05328 ilike';
SELECT 'ilike, no index', count() FROM tab WHERE s ILIKE '%NEEDLE%' SETTINGS use_skip_indexes = 0;
-- An exact token is unaffected.
SELECT 'token', count() FROM tab WHERE hasToken(s, 'needlefish') SETTINGS log_comment = '05328 token';

-- A second part, then a merge: the merged part builds its substring index from the merged dictionary.
INSERT INTO tab SELECT number + 2000, concat('w', toString(number * 7919 % 100003), ' ', if(number % 41 = 0, 'needlefish', 'other'), ' ', lower(hex(cityHash64(number + 2000))))
    FROM numbers(2000);
SELECT 'two parts', count() FROM tab WHERE s LIKE '%needle%' SETTINGS log_comment = '05328 two parts';
SELECT 'two parts, no index', count() FROM tab WHERE s LIKE '%needle%' SETTINGS use_skip_indexes = 0;
SYSTEM START MERGES tab;
OPTIMIZE TABLE tab FINAL;
SELECT 'merged', count() FROM tab WHERE s LIKE '%needle%' SETTINGS log_comment = '05328 merged';
SELECT 'merged, no index', count() FROM tab WHERE s LIKE '%needle%' SETTINGS use_skip_indexes = 0;
SELECT 'merged hex', count() FROM tab WHERE s LIKE '%a1b2%';
SELECT 'merged hex, no index', count() FROM tab WHERE s LIKE '%a1b2%' SETTINGS use_skip_indexes = 0;
SELECT 'parts', count() FROM system.parts WHERE database = currentDatabase() AND table = 'tab' AND active;

SYSTEM FLUSH LOGS query_log;

-- Searches answered from the substring index, and dictionary blocks read: one per part for `needle`, which is one token;
-- none for the absent needle; `match`, the case-insensitive pattern, the prefix pattern and the exact token do not use it.
SELECT log_comment, ProfileEvents['TextIndexSubstringIndexSearches'] AS searches, ProfileEvents['TextIndexReadDictionaryBlocks'] AS blocks_read
    FROM system.query_log
    WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment LIKE '05328 %'
      AND log_comment NOT IN ('05328 ilike', '05328 hex', '05328 common', '05328 prefix')
    ORDER BY event_time_microseconds;
-- The case-insensitive pattern, the prefix pattern and the needle above the threshold read more blocks than the substring index would have picked.
SELECT log_comment, ProfileEvents['TextIndexSubstringIndexSearches'], ProfileEvents['TextIndexReadDictionaryBlocks'] > 10
    FROM system.query_log
    WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment IN ('05328 ilike', '05328 prefix', '05328 common')
    ORDER BY log_comment;
-- The substring index of a part is loaded once and then served from the cache.
SELECT 'cache', sum(ProfileEvents['TextIndexSubstringIndexCacheMisses']) <= 3, sum(ProfileEvents['TextIndexSubstringIndexCacheHits']) > 0
    FROM system.query_log
    WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment LIKE '05328 %';


-- A dictionary text whose length is a multiple of the occurrence block of the substring index: one token of 510 bytes
-- plus a separator and a terminator is 512 bytes.
DROP TABLE IF EXISTS tab_edge;
CREATE TABLE tab_edge (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
INSERT INTO tab_edge VALUES (1, repeat('x', 510));
SELECT 'edge 512', count() FROM tab_edge WHERE s LIKE '%xxxx%';
SELECT 'edge 512, no index', count() FROM tab_edge WHERE s LIKE '%xxxx%' SETTINGS use_skip_indexes = 0;
SELECT 'edge 512 absent', count() FROM tab_edge WHERE s LIKE '%yyyy%';
DROP TABLE tab_edge;

-- A token holding the byte 0x00 or 0x01 cannot be indexed, so such a part is written without a substring index and the
-- scan answers its patterns; the `array` tokenizer on a `String` column keeps the value as one token, so such bytes can
-- reach the dictionary. An empty token is ordinal 0 of its dictionary and the index is built over it.
DROP TABLE IF EXISTS tab_nul_a;
DROP TABLE IF EXISTS tab_nul_b;
DROP TABLE IF EXISTS tab_nul_c;
CREATE TABLE tab_nul_a (id UInt64, s String, INDEX ix s TYPE text(tokenizer = array, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
CREATE TABLE tab_nul_b (id UInt64, s String, INDEX ix s TYPE text(tokenizer = array, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
CREATE TABLE tab_nul_c (id UInt64, s String, INDEX ix s TYPE text(tokenizer = array, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
INSERT INTO tab_nul_a VALUES (1, concat('a', char(0), 'b')), (2, 'needlefish');
INSERT INTO tab_nul_b VALUES (1, concat('c', char(1), 'd')), (2, 'needlefish');
INSERT INTO tab_nul_c VALUES (1, ''), (2, 'needlefish'), (3, 'other');
SELECT 'nul a', count() FROM tab_nul_a WHERE s LIKE '%eedle%' SETTINGS log_comment = '05328 nul a';
SELECT 'nul b', count() FROM tab_nul_b WHERE s LIKE '%eedle%' SETTINGS log_comment = '05328 nul b';
SELECT 'nul b structural', count() FROM tab_nul_b WHERE s LIKE concat('%c', char(1), 'd%');
SELECT 'nul c', count() FROM tab_nul_c WHERE s LIKE '%eedle%' SETTINGS log_comment = '05328 nul c';
-- A needle with a structural byte matches no token of an indexed part, and must not match across a token boundary.
SELECT 'nul c structural', count() FROM tab_nul_c WHERE s LIKE concat('%', char(1), 'xyz%') SETTINGS log_comment = '05328 nul c structural';
SELECT 'nul c empty', count() FROM tab_nul_c WHERE s = '';
DROP TABLE tab_nul_a;
DROP TABLE tab_nul_b;
DROP TABLE tab_nul_c;

-- The block of a match comes from the order of the dictionary: 1025 tokens in a known order, one per row. The tokens at
-- the block boundaries carry a unique tail so that the match sits at the depth the locate has to handle:
-- 510 is 64 bytes and the needle is its last byte, 64 steps to the separator, the most the walk may take;
-- 511 is 64 bytes with a 2-byte tail, 63 steps; 512 is 65 bytes with the tail on its sample, 0 steps;
-- 1023 is 129 bytes with the tail on its second sample; 1024 is short. Each needle is in one token, so one block is read.
DROP TABLE IF EXISTS tab_boundary;
CREATE TABLE tab_boundary (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1, dictionary_block_size = 512) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
INSERT INTO tab_boundary SELECT number, concat('k', leftPad(toString(number), 4, '0'),
    multiIf(number = 510, concat(repeat('m', 58), 'Q'), number = 511, concat(repeat('m', 57), 'qx'), number = 512, concat(repeat('n', 58), 'qy'),
            number = 1023, concat(repeat('p', 122), 'qz'), number = 1024, 'qw', '')) FROM numbers(1025);
SELECT 'boundary 510', count() FROM tab_boundary WHERE s LIKE '%Q%' SETTINGS text_index_like_min_pattern_length = 1, log_comment = '05328 boundary 510';
SELECT 'boundary 511', count() FROM tab_boundary WHERE s LIKE '%qx%' SETTINGS text_index_like_min_pattern_length = 1, log_comment = '05328 boundary 511';
SELECT 'boundary 512', count() FROM tab_boundary WHERE s LIKE '%qy%' SETTINGS text_index_like_min_pattern_length = 1, log_comment = '05328 boundary 512';
SELECT 'boundary 1023', count() FROM tab_boundary WHERE s LIKE '%qz%' SETTINGS text_index_like_min_pattern_length = 1, log_comment = '05328 boundary 1023';
SELECT 'boundary 1024', count() FROM tab_boundary WHERE s LIKE '%qw%' SETTINGS text_index_like_min_pattern_length = 1, log_comment = '05328 boundary 1024';
SELECT 'boundary lengths', countIf(length(s) = 64), countIf(length(s) = 65), countIf(length(s) = 129), uniqExact(s) FROM tab_boundary;
SELECT 'boundary, no index', countIf(s LIKE '%Q%'), countIf(s LIKE '%qx%'), countIf(s LIKE '%qy%'), countIf(s LIKE '%qz%'), countIf(s LIKE '%qw%') FROM tab_boundary SETTINGS use_skip_indexes = 0;
DROP TABLE tab_boundary;

-- The occurrence threshold: `needle` is in two tokens of this part, so a threshold of one sends it to the scan and a threshold of two does not.
DROP TABLE IF EXISTS tab_two;
CREATE TABLE tab_two (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
INSERT INTO tab_two VALUES (1, 'needlefish'), (2, 'needleless'), (3, 'other');
SELECT 'threshold 1', count() FROM tab_two WHERE s LIKE '%needle%' SETTINGS text_index_substring_index_max_occurrences = 1, log_comment = '05328 threshold 1';
SELECT 'threshold 2', count() FROM tab_two WHERE s LIKE '%needle%' SETTINGS text_index_substring_index_max_occurrences = 2, log_comment = '05328 threshold 2';
DROP TABLE tab_two;

-- The third writer of the substring index: an index added and materialized by a mutation.
DROP TABLE IF EXISTS tab_mat;
CREATE TABLE tab_mat (id UInt64, s String) ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
INSERT INTO tab_mat VALUES (1, 'needlefish swims'), (2, 'other');
ALTER TABLE tab_mat ADD INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1) GRANULARITY 1;
ALTER TABLE tab_mat MATERIALIZE INDEX ix SETTINGS mutations_sync = 2;
SELECT 'materialized', count() FROM tab_mat WHERE s LIKE '%eedle%' SETTINGS log_comment = '05328 materialized';
DROP TABLE tab_mat;

-- A dictionary above the size cap gets no substring index either.
DROP TABLE IF EXISTS tab_cap;
CREATE TABLE tab_cap (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1, text_index_substring_index_max_dictionary_size = 16;
INSERT INTO tab_cap VALUES (1, 'needlefish swims'), (2, 'other');
SELECT 'cap', count() FROM tab_cap WHERE s LIKE '%eedle%' SETTINGS log_comment = '05328 cap';
DROP TABLE tab_cap;

-- The serialization version setting is a preference: an index that asks for a substring index gets one, and the part is written with the version that holds it.
DROP TABLE IF EXISTS tab_v2;
CREATE TABLE tab_v2 (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1, text_index_serialization_version = 'v2_with_positions';
INSERT INTO tab_v2 VALUES (1, 'needlefish swims'), (2, 'other');
SELECT 'v2', count() FROM tab_v2 WHERE s LIKE '%eedle%' SETTINGS log_comment = '05328 v2';
DROP TABLE tab_v2;

SYSTEM FLUSH LOGS query_log;
SELECT log_comment, ProfileEvents['TextIndexSubstringIndexSearches']
    FROM system.query_log
    WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish'
      AND log_comment IN ('05328 cap', '05328 v2', '05328 threshold 1', '05328 threshold 2', '05328 materialized')
    ORDER BY log_comment;
SELECT log_comment, ProfileEvents['TextIndexSubstringIndexSearches'], ProfileEvents['TextIndexReadDictionaryBlocks']
    FROM system.query_log
    WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND (log_comment LIKE '05328 boundary %' OR log_comment LIKE '05328 nul %')
    ORDER BY log_comment;

DROP TABLE tab;
