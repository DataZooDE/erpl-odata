#include "catch.hpp"
#include "duckdb_compat.hpp"
#include "duckdb_compat/filter.hpp"

#include <string>
#include <vector>

namespace dc = duckdb_compat;

namespace {
struct FakeFunction {
    std::string name = "fake";
};
struct FakeFunctionSet {
    std::vector<FakeFunction> functions {{}, {}};
};
struct FakeDescription {
    std::vector<std::string> parameter_names;
};
struct FakeFunctionInfo {
    FakeFunctionSet functions;
    std::vector<FakeDescription> descriptions;
};
struct FakeFilterEntry {
    duckdb::idx_t GetIndex() const { return 0; }
    int Filter() const { return 0; }
};
struct FakeMultiColumnFilterSet {
    std::vector<FakeFilterEntry> entries {{}};
    duckdb::idx_t FilterCount() const { return entries.size(); }
    bool HasMultiColumnFilters() const { return true; }
    auto begin() const { return entries.begin(); }
    auto end() const { return entries.end(); }
};
struct OldResultTypes {
    std::vector<int> types {1};
};
struct NewResultTypes {
    std::vector<int> GetTypes() const { return {2}; }
};
struct BothResultTypes : OldResultTypes {
    std::vector<int> GetTypes() const { return {3}; }
};
struct OldAlias {
    std::string alias;
    void SetAlias(const std::string &value) { alias = value; }
};
struct NewAlias {
    std::string alias;
    NewAlias WithAlias(const std::string &value) { alias = value; return *this; }
};
struct BothAlias : OldAlias {
    bool used_new = false;
    BothAlias WithAlias(const std::string &value) { alias = value; used_new = true; return *this; }
};
struct OldGetter {};
struct NewGetter { int (*get_table_entry)(int) = nullptr; };
struct BothGetter : NewGetter { int (*get_bind_info)(int) = nullptr; };
int FakeGetEntry(int value) { return value; }
struct OldInit { void (*init_global)() = nullptr; };
struct NewInit { void (*init_global)() = nullptr; };
struct PointerInit { OldInit value; const OldInit *operator->() const { return &value; } };
struct BothInit : NewInit { OldInit value; const OldInit *operator->() const { return &value; } };
struct OldVarargs { duckdb::LogicalType varargs; };
struct NewSignature {
    bool added = false;
    template <class NAME>
    void AddArgs(NAME, const duckdb::LogicalType &) { added = true; }
};
struct NewVarargs { NewSignature signature; NewSignature &GetSignature() { return signature; } };
struct BothVarargs : OldVarargs { NewSignature signature; NewSignature &GetSignature() { return signature; } };
struct OldValidity {
    static int &Validity(duckdb::Vector &) { static int value = 1; return value; }
};
struct NewValidity {
    static int &ValidityMutable(duckdb::Vector &) { static int value = 2; return value; }
};
struct BothValidity : OldValidity, NewValidity {};
struct NoComparisonEf {};
struct FakeComparisonEf {
    static int TryGetColumnConstantComparison(const duckdb::BoundFunctionExpression &,
                                              duckdb::ExpressionType &) { return 1; }
    static bool IsOptionalExpression(int value) { return value == 1; }
};
} // namespace

TEST_CASE("compat review guards reject mismatches and multi-column filters", "[compat_review]") {
    FakeFunctionInfo info;
    REQUIRE_NOTHROW(dc::NameFunctionsFromDescriptions(info));
    info.descriptions.resize(1);
    REQUIRE_THROWS_AS(dc::NameFunctionsFromDescriptions(info), duckdb::InternalException);
    info.descriptions.resize(2);
    REQUIRE_NOTHROW(dc::NameFunctionsFromDescriptions(info));

    FakeMultiColumnFilterSet filters;
    REQUIRE_THROWS_AS(dc::detail::ForEachFilter(filters, [](duckdb::idx_t, int) {}, 0),
                      duckdb::NotImplementedException);
}

TEST_CASE("compat dispatch prefers new shapes and keeps old fallbacks", "[compat_review]") {
    REQUIRE(dc::detail::ResultTypes(OldResultTypes {}, 0).at(0) == 1);
    REQUIRE(dc::detail::ResultTypes(NewResultTypes {}, 0).at(0) == 2);
    REQUIRE(dc::detail::ResultTypes(BothResultTypes {}, 0).at(0) == 3);
    REQUIRE(dc::detail::WithAlias(OldAlias {}, "old", 0).alias == "old");
    REQUIRE(dc::detail::WithAlias(NewAlias {}, "new", 0).alias == "new");
    REQUIRE(dc::detail::WithAlias(BothAlias {}, "both", 0).used_new);

    OldGetter old_getter;
    NewGetter new_getter;
    BothGetter both_getter;
    dc::detail::SetTableEntryGetter(old_getter, FakeGetEntry, 0);
    dc::detail::SetTableEntryGetter(new_getter, FakeGetEntry, 0);
    dc::detail::SetTableEntryGetter(both_getter, FakeGetEntry, 0);
    REQUIRE(new_getter.get_table_entry == FakeGetEntry);
    REQUIRE(both_getter.get_table_entry == FakeGetEntry);
    REQUIRE_FALSE(dc::detail::HasInitGlobal(PointerInit {}, 0));
    REQUIRE_FALSE(dc::detail::HasInitGlobal(NewInit {}, 0));
    REQUIRE_FALSE(dc::detail::HasInitGlobal(BothInit {}, 0));

    const auto type = duckdb::LogicalType::INTEGER;
    OldVarargs old_varargs;
    NewVarargs new_varargs;
    BothVarargs both_varargs;
    dc::detail::SetVarArgs(old_varargs, type, 0);
    dc::detail::SetVarArgs(new_varargs, type, 0);
    dc::detail::SetVarArgs(both_varargs, type, 0);
    REQUIRE(old_varargs.varargs == type);
    REQUIRE(new_varargs.signature.added);
    REQUIRE(both_varargs.signature.added);

    duckdb::Vector vector(type);
    REQUIRE(dc::detail::MutableValidity<OldValidity>(vector, 0) == 1);
    REQUIRE(dc::detail::MutableValidity<NewValidity>(vector, 0) == 2);
    REQUIRE(dc::detail::MutableValidity<BothValidity>(vector, 0) == 2);
    STATIC_REQUIRE_FALSE(dc::detail::HasColumnConstantComparison<NoComparisonEf>::value);
    STATIC_REQUIRE(dc::detail::HasColumnConstantComparison<FakeComparisonEf>::value);
    REQUIRE(dc::detail::IsOptionalExpression<FakeComparisonEf>(1, 0));
    REQUIRE_FALSE(dc::detail::IsOptionalExpression<NoComparisonEf>(1, 0));

    const std::vector<int> children {1, 2};
    auto optional = [](int value) { return dc::detail::IsOptionalExpression<FakeComparisonEf>(value, 0); };
    auto convert = [](int) -> std::optional<dc::FilterExpressionNode> {
        return dc::FilterExpressionNode {dc::FilterExpressionNode::Kind::COMPARISON,
                                         duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value(), {}};
    };
    auto and_tree = dc::detail::FoldConjunction(dc::FilterExpressionNode::Kind::CONJUNCTION_AND,
                                                children, optional, convert);
    REQUIRE(and_tree.has_value());
    REQUIRE(and_tree->children.size() == 1);
    REQUIRE_FALSE(dc::detail::FoldConjunction(dc::FilterExpressionNode::Kind::CONJUNCTION_OR,
                                              children, optional, convert).has_value());
}
