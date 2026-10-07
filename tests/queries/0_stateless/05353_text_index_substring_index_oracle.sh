#!/usr/bin/env bash
# Tags: no-parallel-replicas
# Tag no-parallel-replicas -- the profile events are read from the query_log of the initiator

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# The substring index against the plain scan and against no index, on a dictionary with short tokens, hex tokens,
# numbers and runs of one letter up to 130 bytes long, so that matches sit deep inside long tokens as well.
$CLICKHOUSE_CLIENT -m -q "
DROP TABLE IF EXISTS tab_oracle;
CREATE TABLE tab_oracle (id UInt64, s String, INDEX ix s TYPE text(tokenizer = splitByNonAlpha, substring_index = 1, dictionary_block_size = 32) GRANULARITY 1)
    ENGINE = MergeTree ORDER BY id SETTINGS allow_experimental_text_index_substring_index = 1;
SYSTEM STOP MERGES tab_oracle;
INSERT INTO tab_oracle SELECT number, concat(lower(hex(cityHash64(number))), ' ', repeat(char(97 + number % 26), 1 + number % 130), ' ', toString(number * 7919 % 100003), ' w', toString(number % 977))
    FROM numbers(3000);
INSERT INTO tab_oracle SELECT number + 3000, concat(lower(hex(cityHash64(number + 3000))), ' ', repeat(char(97 + number % 26), 1 + number % 130), ' x', toString(number % 613))
    FROM numbers(3000);
"

needles="aaaa zzzz abcd ffff 0000 7919 1234 w976 x612 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc qqqq1 deadbeef 00003 8888"

check()
{
    for needle in $needles
    do
        # The substring index with no occurrence threshold, the dictionary scan, and no index at all, which is the oracle.
        with=$($CLICKHOUSE_CLIENT -q "SELECT count() FROM tab_oracle WHERE s LIKE '%$needle%' SETTINGS use_query_condition_cache = 0, use_text_index_like_evaluation_by_dictionary_scan = 1, text_index_substring_index_max_occurrences = 1000000, log_comment = '05353 $1 $needle'")
        scan=$($CLICKHOUSE_CLIENT -q "SELECT count() FROM tab_oracle WHERE s LIKE '%$needle%' SETTINGS use_query_condition_cache = 0, use_text_index_like_evaluation_by_dictionary_scan = 1, text_index_substring_index_max_occurrences = 0")
        none=$($CLICKHOUSE_CLIENT -q "SELECT count() FROM tab_oracle WHERE s LIKE '%$needle%' SETTINGS use_query_condition_cache = 0, use_skip_indexes = 0")
        if [ "$with" = "$scan" ] && [ "$with" = "$none" ]
        then
            echo "$1 $needle ok"
        else
            echo "$1 $needle MISMATCH index=$with scan=$scan none=$none"
        fi
    done
}

check "two parts"
$CLICKHOUSE_CLIENT -q "SYSTEM START MERGES tab_oracle"
$CLICKHOUSE_CLIENT -q "OPTIMIZE TABLE tab_oracle FINAL"
check "merged"

# Every needle of the first leg went through the substring index.
$CLICKHOUSE_CLIENT -m -q "
SYSTEM FLUSH LOGS query_log;
SELECT countIf(ProfileEvents['TextIndexSubstringIndexSearches'] > 0), count()
    FROM system.query_log
    WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment LIKE '05353 %';
DROP TABLE tab_oracle;
"
