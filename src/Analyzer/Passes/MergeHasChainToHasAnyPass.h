#pragma once

#include <Analyzer/IQueryTreePass.h>

namespace DB
{

/** Merge `has`, `hasAll` and `notHas` calls with one constant element on the same array into a single `hasAny`:
  * `NOT has(arr, 'a') AND NOT hasAll(arr, ['b'])` becomes `NOT hasAny(arr, ['a', 'b'])`,
  * `has(arr, 'a') OR hasAll(arr, ['b'])` becomes `hasAny(arr, ['a', 'b'])`.
  * A text index answers one `hasAny` once per block, a chain of calls once per call.
  * Only `WHERE`, `PREWHERE` and `JOIN ON` are rewritten; a chain in the projection, `GROUP BY`, `HAVING` or
  * `ORDER BY` may be made of grouping keys, which have to stay as written.
  */
class MergeHasChainToHasAnyPass final : public IQueryTreePass
{
public:
    String getName() override { return "MergeHasChainToHasAny"; }

    String getDescription() override { return "Merge has and hasAll calls with one constant element on the same array into hasAny"; }

    void run(QueryTreeNodePtr & query_tree_node, ContextPtr context) override;
};

}
