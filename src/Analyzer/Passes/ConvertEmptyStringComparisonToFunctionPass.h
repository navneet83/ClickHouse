#pragma once

#include <Analyzer/IQueryTreePass.h>

namespace DB
{

/// Convert expressions like `column = ''` or `'' = column` to `empty(column)`,
/// and `column != ''` or `'' != column` to `notEmpty(column)`.
///
/// Keys and skip indexes match a query by the exact text of their expression, so rewriting `s = ''`
/// to `empty(s)` would break them. To avoid this, the rewrite is skipped when a key or skip index of
/// a table in the query checks for an empty string, for example `INDEX idx if(s = '', 'abc', s) TYPE minmax`.
class ConvertEmptyStringComparisonToFunctionPass final : public IQueryTreePass
{
public:
    String getName() override { return "ConvertEmptyStringComparisonToFunction"; }

    String getDescription() override { return "Convert comparisons to '' into empty() / notEmpty() functions."; }

    void run(QueryTreeNodePtr & query_tree_node, ContextPtr context) override;
};

}
