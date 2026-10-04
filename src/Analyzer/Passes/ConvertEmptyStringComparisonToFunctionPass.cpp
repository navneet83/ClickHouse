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
#include <Parsers/ASTIdentifier.h>
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

void collectIdentifiers(const ASTPtr & ast, NameSet & names)
{
    if (const auto * identifier = ast->as<ASTIdentifier>())
        names.insert(identifier->shortName());
    for (const auto & child : ast->children)
        collectIdentifiers(child, names);
}

/// Collects the columns that the expression compares with `''` anywhere, as `x = ''` or `x != ''` with the literal
/// on either side. For `if(s = '', 'abc', s)` that is `s`.
void collectColumnsComparedWithEmptyString(const ASTPtr & ast, NameSet & columns)
{
    if (const auto * function = ast->as<ASTFunction>();
        function && (function->name == "equals" || function->name == "notEquals") && function->arguments
        && function->arguments->children.size() == 2)
    {
        const auto & args = function->arguments->children;
        if (isEmptyStringLiteral(args[0]))
            collectIdentifiers(args[1], columns);
        else if (isEmptyStringLiteral(args[1]))
            collectIdentifiers(args[0], columns);
    }

    for (const auto & child : ast->children)
        collectColumnsComparedWithEmptyString(child, columns);
}

/// The columns that a key, skip index or projection of the table compares with `''`. A projection is checked as
/// a whole, since its filter and its own skip indexes are matched by their written text just like its keys.
/// Skip index expressions are stored with ALIAS columns already expanded, so aliases need no separate check.
NameSet columnsComparedWithEmptyStringInDeclarations(const StorageInMemoryMetadata & metadata)
{
    NameSet columns;
    for (const auto * key : {&metadata.getPartitionKey(), &metadata.getSortingKey(), &metadata.getPrimaryKey(), &metadata.getSamplingKey()})
        if (key->expression_list_ast)
            collectColumnsComparedWithEmptyString(key->expression_list_ast, columns);

    for (const auto & index : metadata.getSecondaryIndices())
        if (index.expression_list_ast)
            collectColumnsComparedWithEmptyString(index.expression_list_ast, columns);

    for (const auto & projection : metadata.getProjections())
        if (projection.definition_ast)
            collectColumnsComparedWithEmptyString(projection.definition_ast, columns);

    return columns;
}

/// The table a column is read from, through the wrappers that forward the read to another table: a lazily loaded
/// table, an Alias, a materialized view and a Buffer. The loop is bounded in case the wrappers form a cycle.
/// Distributed, Merge and View are not followed: they run the already rewritten query on their tables,
/// so an index behind them cannot be helped here.
StoragePtr getUnderlyingStorage(const QueryTreeNodePtr & source)
{
    StoragePtr storage;
    if (const auto * table_node = source->as<TableNode>())
        storage = table_node->getStorage();
    else if (const auto * table_function_node = source->as<TableFunctionNode>())
        storage = table_function_node->getStorage();

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
    return storage;
}

void collectColumnNodes(const QueryTreeNodePtr & node, std::vector<const ColumnNode *> & columns)
{
    if (const auto * column_node = node->as<ColumnNode>())
        columns.push_back(column_node);
    for (const auto & child : node->getChildren())
        if (child)
            collectColumnNodes(child, columns);
}

class ConvertEmptyStringComparisonToFunctionVisitor
    : public InDepthQueryTreeVisitorWithContext<ConvertEmptyStringComparisonToFunctionVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<ConvertEmptyStringComparisonToFunctionVisitor>;

    using Base::Base;

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

        if (mustStayAsWritten(expr_node))
            return;

        const String replacement_func = (func_name == "equals") ? "empty" : "notEmpty";

        auto replacement_node = std::make_shared<FunctionNode>(replacement_func);
        replacement_node->getArguments().getNodes().push_back(expr_node);

        resolveOrdinaryFunctionNodeByName(*replacement_node, replacement_func, getContext());

        node = std::move(replacement_node);
    }

private:
    /// Whether a column of the expression belongs to a table whose key, skip index or projection compares that
    /// column with `''`. Such a comparison must keep its written text, or the index no longer matches the query.
    bool mustStayAsWritten(const QueryTreeNodePtr & expr)
    {
        std::vector<const ColumnNode *> columns;
        collectColumnNodes(expr, columns);

        for (const auto * column_node : columns)
        {
            auto source = column_node->getColumnSourceOrNull();
            if (!source)
                continue;

            auto storage = getUnderlyingStorage(source);
            if (!storage)
                continue;

            auto [it, inserted] = columns_by_storage.try_emplace(storage.get());
            if (inserted)
            {
                auto metadata = storage->getInMemoryMetadataPtr(getContext(), false);
                it->second = columnsComparedWithEmptyStringInDeclarations(*metadata);
            }

            if (it->second.contains(column_node->getColumnName()))
                return true;
        }
        return false;
    }

    /// Looked up once per table and query: most queries never reach this.
    std::unordered_map<const IStorage *, NameSet> columns_by_storage;
};

}

void ConvertEmptyStringComparisonToFunctionPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    ConvertEmptyStringComparisonToFunctionVisitor visitor(std::move(context));
    visitor.visit(query_tree_node);
}

}
