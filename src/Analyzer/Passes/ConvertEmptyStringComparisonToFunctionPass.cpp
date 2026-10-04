#include <Analyzer/Passes/ConvertEmptyStringComparisonToFunctionPass.h>

#include <Analyzer/ColumnNode.h>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/Utils.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeString.h>
#include <Functions/FunctionFactory.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Storages/StorageAlias.h>
#include <Storages/StorageBuffer.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/StorageMaterializedView.h>
#include <Storages/StorageProxy.h>
#include <Common/FieldVisitors.h>

namespace DB
{

namespace Setting
{
    extern const SettingsBool optimize_empty_string_comparisons;
}

namespace
{

/// The literal `''`.
bool isEmptyStringLiteral(const ASTPtr & ast)
{
    const auto * literal = ast->as<ASTLiteral>();
    return literal && literal->value.getType() == Field::Types::String && literal->value.safeGet<String>().empty();
}

/// Whether the expression contains `x = ''` or `x != ''` anywhere, with the literal on either side.
bool comparesWithEmptyString(const ASTPtr & ast)
{
    if (const auto * function = ast->as<ASTFunction>();
        function && (function->name == "equals" || function->name == "notEquals") && function->arguments
        && function->arguments->children.size() == 2
        && (isEmptyStringLiteral(function->arguments->children[0]) || isEmptyStringLiteral(function->arguments->children[1])))
        return true;

    for (const auto & child : ast->children)
        if (comparesWithEmptyString(child))
            return true;
    return false;
}

/// Whether the partition, sorting, primary or sampling key compares with `''`.
bool keysDeclareComparisonWithEmptyString(const StorageInMemoryMetadata & metadata)
{
    for (const auto * key : {&metadata.getPartitionKey(), &metadata.getSortingKey(), &metadata.getPrimaryKey(), &metadata.getSamplingKey()})
        if (key->expression_list_ast && comparesWithEmptyString(key->expression_list_ast))
            return true;
    return false;
}

/// Whether any key, skip index or projection of the table compares with `''`. A projection is checked as a whole,
/// since its filter and its own skip indexes are matched by their written text just like its keys.
/// Skip index expressions are stored with ALIAS columns already expanded, so aliases need no separate check.
bool declaresComparisonWithEmptyString(const StorageInMemoryMetadata & metadata)
{
    if (keysDeclareComparisonWithEmptyString(metadata))
        return true;

    for (const auto & index : metadata.getSecondaryIndices())
        if (index.expression_list_ast && comparesWithEmptyString(index.expression_list_ast))
            return true;

    for (const auto & projection : metadata.getProjections())
        if (projection.definition_ast && comparesWithEmptyString(projection.definition_ast))
            return true;

    return false;
}

/// Walks the query tree, including subqueries, and sets `found` if any table it reads has a key or skip index
/// that compares with `''`. Such a table needs the comparison as written, so the rewrite is skipped for the whole query.
class FindTableWithIndexedEmptyStringComparisonVisitor : public InDepthQueryTreeVisitorWithContext<FindTableWithIndexedEmptyStringComparisonVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<FindTableWithIndexedEmptyStringComparisonVisitor>;
    using Base::Base;

    bool found = false;

    void enterImpl(QueryTreeNodePtr & node)
    {
        StoragePtr storage;
        if (const auto * table_node = node->as<TableNode>())
            storage = table_node->getStorage();
        else if (const auto * table_function_node = node->as<TableFunctionNode>())
            storage = table_function_node->getStorage();
        if (!storage)
            return;

        /// A lazily loaded table, an Alias, a materialized view and a Buffer forward the read to another table,
        /// so look at that table's indexes. The loop is bounded in case the wrappers form a cycle.
        /// Distributed, Merge and View are not followed: they run the already rewritten query on their tables,
        /// so an index behind them cannot be helped here.
        for (size_t i = 0; storage && i < 16; ++i)
        {
            if (const auto * proxy = dynamic_cast<const StorageProxy *>(storage.get()))
                storage = proxy->getNested();
            else if (const auto * alias = storage->as<StorageAlias>())
                storage = alias->tryGetTargetTable();
            else if (const auto * materialized_view = storage->as<StorageMaterializedView>())
                storage = materialized_view->tryGetTargetTable();
            else if (const auto * buffer = storage->as<StorageBuffer>())
                storage = buffer->getDestinationTable();
            else
                break;
        }
        if (!storage)
            return;

        const auto metadata = storage->getInMemoryMetadataPtr(getContext(), false);
        if (declaresComparisonWithEmptyString(*metadata))
            found = true;
    }
};

class ConvertEmptyStringComparisonToFunctionVisitor
    : public InDepthQueryTreeVisitorWithContext<ConvertEmptyStringComparisonToFunctionVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<ConvertEmptyStringComparisonToFunctionVisitor>;

    ConvertEmptyStringComparisonToFunctionVisitor(ContextPtr context_, QueryTreeNodePtr root_)
        : Base(std::move(context_))
        , root(std::move(root_))
    {
    }

    void enterImpl(QueryTreeNodePtr & node)
    {
        if (!getSettings()[Setting::optimize_empty_string_comparisons])
            return;

        auto * function_node = node->as<FunctionNode>();
        if (!function_node)
            return;

        const String & func_name = function_node->getFunctionName();
        if (func_name != "equals" && func_name != "notEquals")
            return;

        const auto & args = function_node->getArguments().getNodes();
        if (args.size() != 2)
            return;

        // Identify which argument is the empty string literal
        int const_idx = -1;

        for (size_t i = 0; i < 2; ++i)
        {
            if (const auto * constant_node = args[i]->as<ConstantNode>())
            {
                if (isStringOrFixedString(constant_node->getResultType()))
                {
                    const Field & val = constant_node->getValue();
                    if (val.getType() == Field::Types::String && val.safeGet<String>().empty())
                    {
                        const_idx = static_cast<int>(i);
                        break;
                    }
                }
            }
        }

        if (const_idx == -1)
            return;

        size_t expr_idx = 1 - const_idx;
        const auto & expr_node = args[expr_idx];

        const auto expr_type = expr_node->getResultType();
        if (!expr_type || !isStringOrFixedString(expr_type))
            return;

        /// Look at the tables only once, and only when there is something to rewrite: most queries never get here.
        if (!tables_checked)
        {
            FindTableWithIndexedEmptyStringComparisonVisitor find_indexed_comparison(getContext());
            find_indexed_comparison.visit(root);
            keep_comparisons_as_written = find_indexed_comparison.found;
            tables_checked = true;
        }
        if (keep_comparisons_as_written)
            return;

        const String replacement_func = (func_name == "equals") ? "empty" : "notEmpty";

        auto replacement_node = std::make_shared<FunctionNode>(replacement_func);
        replacement_node->getArguments().getNodes().push_back(expr_node);

        resolveOrdinaryFunctionNodeByName(*replacement_node, replacement_func, getContext());

        node = std::move(replacement_node);
    }

private:
    QueryTreeNodePtr root;
    bool tables_checked = false;
    bool keep_comparisons_as_written = false;
};

}

void ConvertEmptyStringComparisonToFunctionPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    ConvertEmptyStringComparisonToFunctionVisitor visitor(std::move(context), query_tree_node);
    visitor.visit(query_tree_node);
}

}
