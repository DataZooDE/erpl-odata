#include "catch.hpp"

#include "odata_predicate_pushdown_helper.hpp"
#include "odata_url_helpers.hpp"

#include "duckdb_compat/filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/struct_filter.hpp"
// DuckDB 1.5 added bloom filters; the 1.4 LTS line this extension also builds against has
// no such header or filter type, so the cases exercising them are compiled conditionally.
#ifdef ERPL_TEST_BLOOM_FILTER
#include "duckdb/planner/filter/bloom_filter.hpp"
namespace duckdb { class BFTableFilter; class LegacyBFTableFilter; }
namespace {
using TestBloomFilter = std::conditional_t<duckdb_compat::NEW_TABLE_FILTERS,
                                          duckdb::LegacyBFTableFilter, duckdb::BFTableFilter>;
// DuckDB 2.0.0 takes the bloom filter as shared_ptr<const BloomFilter>; earlier lines
// (and the 2.0 pre-release) take a BloomFilter&. Pick whichever constructor exists.
template <class F = TestBloomFilter>
auto MakeTestBloomFilter(int)
    -> decltype(duckdb::make_uniq<F>(duckdb::shared_ptr<const duckdb::BloomFilter>(), false, std::string(),
                                     duckdb::LogicalType())) {
	return duckdb::make_uniq<F>(duckdb::make_shared_ptr<duckdb::BloomFilter>(), false, "Col",
	                            duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER));
}
template <class F = TestBloomFilter>
duckdb::unique_ptr<F> MakeTestBloomFilter(long) {
	static duckdb::BloomFilter bloom;
	return duckdb::make_uniq<F>(bloom, false, "Col", duckdb::LogicalType(duckdb::LogicalTypeId::INTEGER));
}
}
#endif
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

using namespace erpl_web;
using duckdb::Value;

namespace {

// The helper percent-encodes the generated expression, so decode it back before
// asserting. Returns the bare $filter expression, or "" when none was produced.
std::string FilterExpression(const ODataPredicatePushdownHelper &helper) {
	auto clause = helper.FilterClause();
	if (clause.empty()) {
		return "";
	}
	const std::string prefix = "$filter=";
	REQUIRE(clause.rfind(prefix, 0) == 0);
	return ODataUrlCodec::decodeQueryValue(clause.substr(prefix.size()));
}

// Builds a helper over a single column and feeds it one filter.
std::string TranslateOne(duckdb::unique_ptr<duckdb::TableFilter> filter,
                         ODataVersion version,
                         const std::string &column_name = "Col") {
	ODataPredicatePushdownHelper helper({column_name});
	helper.SetODataVersion(version);

	duckdb::TableFilterSet filter_set;
	duckdb_compat::SetFilter(filter_set, 0, std::move(filter));
	helper.ConsumeFilters(&filter_set);

	return FilterExpression(helper);
}

duckdb::unique_ptr<duckdb_compat::ConstantFilter> MakeEq(Value value) {
	return duckdb::make_uniq<duckdb_compat::ConstantFilter>(duckdb::ExpressionType::COMPARE_EQUAL, std::move(value));
}

} // namespace

// ---------------------------------------------------------------------------
// GitHub #59 - Top-N placeholder sentinel must never reach the server
// ---------------------------------------------------------------------------

TEST_CASE("An uninitialized dynamic filter produces no $filter", "[odata_filter]") {
	// DuckDB's Top-N optimizer installs an OptionalFilter(DynamicFilter) whose
	// filter_data holds a placeholder sentinel while `initialized` is still
	// false. Shipping that sentinel produces "$filter=Col lt -2147483648" and
	// silently returns zero rows, with no residual filter able to restore them.
	auto filter_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN,
	    Value::INTEGER(duckdb::NumericLimits<int32_t>::Minimum()), false);

	auto dynamic_filter = duckdb::make_uniq<duckdb_compat::DynamicFilter>(filter_data);
	auto optional_filter = duckdb::make_uniq<duckdb_compat::OptionalFilter>(std::move(dynamic_filter));

	REQUIRE(TranslateOne(std::move(optional_filter), ODataVersion::V4) == "");
}

TEST_CASE("An initialized dynamic filter is translated normally", "[odata_filter]") {
	auto filter_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN, Value::INTEGER(42), true);

	auto dynamic_filter = duckdb::make_uniq<duckdb_compat::DynamicFilter>(filter_data);
	auto optional_filter = duckdb::make_uniq<duckdb_compat::OptionalFilter>(std::move(dynamic_filter));

	REQUIRE(TranslateOne(std::move(optional_filter), ODataVersion::V4) == "Col lt 42");
}

// ---------------------------------------------------------------------------
// GitHub #65 - single quotes inside string literals must be doubled
// ---------------------------------------------------------------------------

TEST_CASE("V4 string literals double an embedded single quote", "[odata_filter]") {
	REQUIRE(TranslateOne(MakeEq(Value("O'Brien")), ODataVersion::V4) == "Col eq 'O''Brien'");
}

TEST_CASE("V2 string literals double an embedded single quote", "[odata_filter]") {
	REQUIRE(TranslateOne(MakeEq(Value("O'Brien")), ODataVersion::V2) == "Col eq 'O''Brien'");
}

// ---------------------------------------------------------------------------
// GitHub #64 - version-aware literal formatting per EDM type
// ---------------------------------------------------------------------------

TEST_CASE("Narrow and unsigned integers are emitted unquoted", "[odata_filter]") {
	REQUIRE(TranslateOne(MakeEq(Value::SMALLINT(5)), ODataVersion::V4) == "Col eq 5");
	REQUIRE(TranslateOne(MakeEq(Value::TINYINT(5)), ODataVersion::V4) == "Col eq 5");
	REQUIRE(TranslateOne(MakeEq(Value::UBIGINT(5)), ODataVersion::V4) == "Col eq 5");
	REQUIRE(TranslateOne(MakeEq(Value::HUGEINT(5)), ODataVersion::V4) == "Col eq 5");
}

TEST_CASE("Floating point values are emitted unquoted", "[odata_filter]") {
	REQUIRE(TranslateOne(MakeEq(Value::FLOAT(1.5f)), ODataVersion::V4) == "Col eq 1.5");
}

TEST_CASE("V4 dates are emitted bare, V2 dates use the datetime literal form", "[odata_filter]") {
	auto date = Value::DATE(duckdb::Date::FromDate(2020, 1, 2));
	REQUIRE(TranslateOne(MakeEq(date), ODataVersion::V4) == "Col eq 2020-01-02");
	REQUIRE(TranslateOne(MakeEq(date), ODataVersion::V2) == "Col eq datetime'2020-01-02T00:00:00'");
}

TEST_CASE("Timestamps use ISO-8601 with a T separator, never a space", "[odata_filter]") {
	auto ts = Value::TIMESTAMP(duckdb::Timestamp::FromDatetime(duckdb::Date::FromDate(2020, 1, 2),
	                                                           duckdb::Time::FromTime(3, 4, 5, 0)));
	auto v4 = TranslateOne(MakeEq(ts), ODataVersion::V4);
	REQUIRE(v4.find(' ') != std::string::npos); // only the separators around "eq"
	REQUIRE(v4 == "Col eq 2020-01-02T03:04:05Z");
	REQUIRE(TranslateOne(MakeEq(ts), ODataVersion::V2) == "Col eq datetime'2020-01-02T03:04:05'");
}

TEST_CASE("GUIDs are bare in V4 and use the guid literal form in V2", "[odata_filter]") {
	auto uuid = Value::UUID("11111111-2222-3333-4444-555555555555");
	REQUIRE(TranslateOne(MakeEq(uuid), ODataVersion::V4) == "Col eq 11111111-2222-3333-4444-555555555555");
	REQUIRE(TranslateOne(MakeEq(uuid), ODataVersion::V2) == "Col eq guid'11111111-2222-3333-4444-555555555555'");
}

TEST_CASE("Booleans are emitted lowercase and unquoted", "[odata_filter]") {
	REQUIRE(TranslateOne(MakeEq(Value::BOOLEAN(true)), ODataVersion::V4) == "Col eq true");
	REQUIRE(TranslateOne(MakeEq(Value::BOOLEAN(false)), ODataVersion::V4) == "Col eq false");
}

// ---------------------------------------------------------------------------
// GitHub #66 - IN filters translate rather than throwing out of init_global
// ---------------------------------------------------------------------------

TEST_CASE("An IN filter becomes an or-chain instead of throwing", "[odata_filter]") {
	duckdb::vector<Value> values{Value::INTEGER(1), Value::INTEGER(2), Value::INTEGER(3)};
	auto in_filter = duckdb::make_uniq<duckdb_compat::InFilter>(std::move(values));

	REQUIRE(TranslateOne(std::move(in_filter), ODataVersion::V4) ==
	        "(Col eq 1 or Col eq 2 or Col eq 3)");
}

// ---------------------------------------------------------------------------
// GitHub #85 - empty child translations must not corrupt a conjunction
// ---------------------------------------------------------------------------

TEST_CASE("An AND conjunction drops advisory children", "[odata_filter]") {
	// An uninitialised dynamic filter is advisory - the Top-N optimiser fills it in
	// later and the query is correct without it - so dropping it is safe. Children
	// that are NOT advisory must not be dropped; see the case below.
	auto conjunction = duckdb::make_uniq<duckdb_compat::ConjunctionAndFilter>();
	conjunction->child_filters.push_back(MakeEq(Value::INTEGER(1)));

	auto sentinel_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN, Value::INTEGER(7), false);
	conjunction->child_filters.push_back(duckdb::make_uniq<duckdb_compat::DynamicFilter>(sentinel_data));

	REQUIRE(TranslateOne(std::move(conjunction), ODataVersion::V4) == "(Col eq 1)");
}

TEST_CASE("An OR conjunction is abandoned when a branch is merely advisory", "[odata_filter]") {
	// An advisory branch carries no predicate of its own, so the disjunction it appears in
	// constrains nothing and pushing a partial form would NARROW the result. Abandoning it
	// is safe here precisely because the branch was advisory; a branch that genuinely
	// cannot be translated throws instead (see the case above).
	auto conjunction = duckdb::make_uniq<duckdb_compat::ConjunctionOrFilter>();
	conjunction->child_filters.push_back(MakeEq(Value::INTEGER(1)));

	auto sentinel_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN, Value::INTEGER(7), false);
	conjunction->child_filters.push_back(duckdb::make_uniq<duckdb_compat::DynamicFilter>(sentinel_data));

	REQUIRE(TranslateOne(std::move(conjunction), ODataVersion::V4) == "");
}

TEST_CASE("A conjunction with no translatable children produces no $filter", "[odata_filter]") {
	auto conjunction = duckdb::make_uniq<duckdb_compat::ConjunctionAndFilter>();

	auto sentinel_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN, Value::INTEGER(7), false);
	conjunction->child_filters.push_back(duckdb::make_uniq<duckdb_compat::DynamicFilter>(sentinel_data));

	REQUIRE(TranslateOne(std::move(conjunction), ODataVersion::V4) == "");
}


// ---------------------------------------------------------------------------
// GitHub #153 - a filter we cannot translate must never be dropped silently
// ---------------------------------------------------------------------------
//
// DuckDB removes a predicate from the plan once it becomes a TableFilter and the
// function advertises filter_pushdown (see optimizer/pushdown/pushdown_get.cpp), so
// nothing re-applies it above the scan. Dropping one therefore returns rows that do
// not satisfy the query's own WHERE clause. Against live SAP this made
// "WHERE CurrencyCode = ''" answer 4136 rows where the correct answer is 0.

TEST_CASE("An empty string comparison is translated, not skipped", "[odata_filter]") {
	// "Col eq ''" is valid OData and SAP Gateway answers it correctly; there was
	// never a reason to drop it.
	REQUIRE(TranslateOne(MakeEq(Value("")), ODataVersion::V4) == "Col eq ''");
	REQUIRE(TranslateOne(MakeEq(Value("")), ODataVersion::V2) == "Col eq ''");
}

TEST_CASE("A long string literal is translated, not skipped", "[odata_filter]") {
	// A long literal is the service's business: a rejection from the server is loud,
	// whereas dropping the filter is silently wrong.
	const std::string long_value(1001, 'x');
	const auto translated = TranslateOne(MakeEq(Value(long_value)), ODataVersion::V4);
	REQUIRE(translated == "Col eq '" + long_value + "'");
}

TEST_CASE("A constant with no OData literal form fails loudly", "[odata_filter]") {
	// TIMESTAMP_TZ has no safe literal form. Failing is the only correct answer left,
	// because DuckDB will not filter the rows for us.
	REQUIRE_THROWS_AS(TranslateOne(duckdb::make_uniq<duckdb_compat::ConstantFilter>(
	                                   duckdb::ExpressionType::COMPARE_EQUAL,
	                                   Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(0))),
	                               ODataVersion::V4),
	                  duckdb::NotImplementedException);
}

TEST_CASE("An AND conjunction fails loudly when a child cannot be translated", "[odata_filter]") {
	// Dropping the child would widen the result set with nothing downstream to narrow
	// it again.
	auto conjunction = duckdb::make_uniq<duckdb_compat::ConjunctionAndFilter>();
	conjunction->child_filters.push_back(MakeEq(Value::INTEGER(1)));
	conjunction->child_filters.push_back(duckdb::make_uniq<duckdb_compat::ConstantFilter>(
	    duckdb::ExpressionType::COMPARE_EQUAL, Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(0))));

	REQUIRE_THROWS_AS(TranslateOne(std::move(conjunction), ODataVersion::V4),
	                  duckdb::NotImplementedException);
}

TEST_CASE("An initialised dynamic filter of an untranslatable type is skipped, not thrown",
          "[odata_filter]") {
	// A dynamic filter comes from the Top-N optimiser and is always enforced again above
	// the scan, so it is advisory whatever it holds. Once initialised it reaches
	// TranslateConstantComparison, which now throws for a type with no literal form -- that
	// would abort "ORDER BY <timestamptz col> LIMIT n", a query that works today.
	auto filter_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN, Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(0)), true);

	REQUIRE(TranslateOne(duckdb::make_uniq<duckdb_compat::DynamicFilter>(filter_data),
	                     ODataVersion::V4) == "");
}

TEST_CASE("An IN filter fails loudly when a value cannot be translated", "[odata_filter]") {
	// Same reasoning as the constant comparison: dropping the filter returns rows that do
	// not satisfy the WHERE clause, and nothing downstream filters them out.
	duckdb::vector<Value> values;
	values.push_back(Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(0)));
	REQUIRE_THROWS_AS(TranslateOne(duckdb::make_uniq<duckdb_compat::InFilter>(std::move(values)),
	                               ODataVersion::V4),
	                  duckdb::NotImplementedException);
}

TEST_CASE("A very long IN list is still translated", "[odata_filter]") {
	// The old 100-value ceiling silently dropped the filter and returned the whole entity
	// set. A long URL is the service's business; a 414 is loud.
	duckdb::vector<Value> values;
	for (int i = 0; i < 101; i++) {
		values.push_back(Value::INTEGER(i));
	}
	const auto translated = TranslateOne(duckdb::make_uniq<duckdb_compat::InFilter>(std::move(values)),
	                                     ODataVersion::V4);
	REQUIRE(translated.find("Col eq 0") != std::string::npos);
	REQUIRE(translated.find("Col eq 100") != std::string::npos);
}

TEST_CASE("An OR conjunction fails loudly when a branch cannot be translated", "[odata_filter]") {
	// Pushing nothing means the server returns everything and DuckDB re-applies nothing,
	// so the query silently widens -- the same defect as dropping an AND child.
	auto conjunction = duckdb::make_uniq<duckdb_compat::ConjunctionOrFilter>();
	conjunction->child_filters.push_back(MakeEq(Value::INTEGER(1)));
	conjunction->child_filters.push_back(duckdb::make_uniq<duckdb_compat::ConstantFilter>(
	    duckdb::ExpressionType::COMPARE_EQUAL, Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(0))));

	REQUIRE_THROWS_AS(TranslateOne(std::move(conjunction), ODataVersion::V4),
	                  duckdb::NotImplementedException);
}

TEST_CASE("A filter on a column that cannot be resolved fails loudly", "[odata_filter]") {
	// The filter is neither pushed nor re-applied, so continuing past it returns rows that
	// do not satisfy the WHERE clause. Unlike the other cases this one fires on an ordinary
	// projection/expand mapping bug rather than on an exotic type, which makes silently
	// widening worse, not better.
	ODataPredicatePushdownHelper helper({"Col"});
	helper.SetODataVersion(ODataVersion::V4);

	duckdb::TableFilterSet filter_set;
	duckdb_compat::SetFilter(filter_set, 7, MakeEq(Value::INTEGER(1)));  // no column at index 7

	// NOT InternalException. DuckDB treats ExceptionType::INTERNAL as fatal -
	// client_context.cpp calls ValidChecker::Invalidate(db_inst) - so every later statement
	// on that database instance fails too. An unresolved column index is reachable from a
	// query (a filter on an expanded column resolves against base names only), so it must
	// fail the statement, not the instance.
	REQUIRE_THROWS_AS(helper.ConsumeFilters(&filter_set), duckdb::NotImplementedException);
}

TEST_CASE("Advisory filters are still skipped rather than failing", "[odata_filter]") {
	// The contrast: these three are documented as not required for correctness, so
	// ignoring them stays legitimate and must not become an error.
	auto sentinel_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN, Value::INTEGER(7), false);
	REQUIRE(TranslateOne(duckdb::make_uniq<duckdb_compat::DynamicFilter>(sentinel_data),
	                     ODataVersion::V4) == "");

	auto child = duckdb::make_uniq<duckdb_compat::ConstantFilter>(duckdb::ExpressionType::COMPARE_EQUAL,
	                                                       Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(0)));
	auto optional_filter = duckdb::make_uniq<duckdb_compat::OptionalFilter>(std::move(child));
	REQUIRE(TranslateOne(std::move(optional_filter), ODataVersion::V4) == "");
}

// ---------------------------------------------------------------------------
// Behaviour that is already correct and must stay that way
// ---------------------------------------------------------------------------

TEST_CASE("IS NULL and IS NOT NULL translate to the null literal", "[odata_filter]") {
	ODataPredicatePushdownHelper helper({"Col"});
	helper.SetODataVersion(ODataVersion::V4);

	duckdb::TableFilterSet filter_set;
	duckdb_compat::SetFilter(filter_set, 0, duckdb::make_uniq<duckdb_compat::IsNullFilter>());
	helper.ConsumeFilters(&filter_set);
	REQUIRE(FilterExpression(helper) == "Col eq null");

	ODataPredicatePushdownHelper not_null_helper({"Col"});
	not_null_helper.SetODataVersion(ODataVersion::V4);
	duckdb::TableFilterSet not_null_set;
	duckdb_compat::SetFilter(not_null_set, 0, duckdb::make_uniq<duckdb_compat::IsNotNullFilter>());
	not_null_helper.ConsumeFilters(&not_null_set);
	REQUIRE(FilterExpression(not_null_helper) == "Col ne null");
}

TEST_CASE("Comparison operators map to their OData spellings", "[odata_filter]") {
	auto translate = [](duckdb::ExpressionType type) {
		return TranslateOne(duckdb::make_uniq<duckdb_compat::ConstantFilter>(type, Value::INTEGER(1)),
		                    ODataVersion::V4);
	};
	REQUIRE(translate(duckdb::ExpressionType::COMPARE_EQUAL) == "Col eq 1");
	REQUIRE(translate(duckdb::ExpressionType::COMPARE_NOTEQUAL) == "Col ne 1");
	REQUIRE(translate(duckdb::ExpressionType::COMPARE_LESSTHAN) == "Col lt 1");
	REQUIRE(translate(duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO) == "Col le 1");
	REQUIRE(translate(duckdb::ExpressionType::COMPARE_GREATERTHAN) == "Col gt 1");
	REQUIRE(translate(duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO) == "Col ge 1");
}

// ---------------------------------------------------------------------------
// An optional filter must never fail the query
// ---------------------------------------------------------------------------

TEST_CASE("An optional filter wrapping an untranslatable child is skipped, not thrown",
          "[odata_filter]") {
	// An optional filter is advisory whatever it wraps, so a child that would otherwise
	// fail the query must not. A struct-extract child stands in for the bloom filter on
	// builds that have no bloom filters.
	auto child = duckdb::make_uniq<duckdb_compat::ConstantFilter>(duckdb::ExpressionType::COMPARE_EQUAL,
	                                                       Value::INTEGER(1));
	auto struct_child = duckdb::make_uniq<duckdb_compat::StructFilter>(0, "field", std::move(child));
	auto optional_filter = duckdb::make_uniq<duckdb_compat::OptionalFilter>(std::move(struct_child));

	REQUIRE(TranslateOne(std::move(optional_filter), ODataVersion::V4) == "");
}

#ifdef ERPL_TEST_BLOOM_FILTER
TEST_CASE("An optional filter wrapping a bloom filter is skipped, not thrown", "[odata_filter]") {
	// DuckDB documents OPTIONAL_FILTER as "executing filter is not required for query
	// correctness". A hash join pushes an optional filter wrapping a bloom filter, so
	// translating the child eagerly and letting it reach the default: arm would make an
	// ordinary join fail outright.
	auto bf_filter = MakeTestBloomFilter(0);
	auto optional_filter = duckdb::make_uniq<duckdb_compat::OptionalFilter>(std::move(bf_filter));

	REQUIRE(TranslateOne(std::move(optional_filter), ODataVersion::V4) == "");
}

TEST_CASE("A bare bloom filter is skipped rather than throwing", "[odata_filter]") {
	auto bf_filter = MakeTestBloomFilter(0);
	REQUIRE(TranslateOne(std::move(bf_filter), ODataVersion::V4) == "");
}
#endif  // ERPL_TEST_BLOOM_FILTER

TEST_CASE("A filter kind that is required for correctness still fails loudly", "[odata_filter]") {
	// The contrast case: unlike an optional filter, a struct-extract filter IS required
	// for correctness, so silently dropping it could lose rows. Failing is the safe answer.
	auto child = duckdb::make_uniq<duckdb_compat::ConstantFilter>(duckdb::ExpressionType::COMPARE_EQUAL,
	                                                       Value::INTEGER(1));
	auto struct_filter = duckdb::make_uniq<duckdb_compat::StructFilter>(0, "field", std::move(child));
	REQUIRE_THROWS_AS(TranslateOne(std::move(struct_filter), ODataVersion::V4),
	                  duckdb::NotImplementedException);
}

// ---------------------------------------------------------------------------
// A row limit must not be pushed over a result the server has not filtered
// ---------------------------------------------------------------------------

TEST_CASE("$top is withheld when a filter could not be translated", "[odata_filter]") {
	// The server would apply $top to the UNFILTERED result and return a short page,
	// so the query would silently produce fewer rows than the caller asked for.
	ODataPredicatePushdownHelper helper({"Col"});
	helper.SetODataVersion(ODataVersion::V4);
	helper.ConsumeLimit(10);

	duckdb::TableFilterSet filter_set;
	// An advisory filter is the only kind that still reaches the server untranslated:
	// everything else now fails the query rather than being dropped (GitHub #153).
	auto sentinel_data = duckdb_compat::MakeDynamicFilterData(duckdb::ExpressionType::COMPARE_LESSTHAN, Value::INTEGER(7), false);
	duckdb_compat::SetFilter(filter_set, 0, duckdb::make_uniq<duckdb_compat::DynamicFilter>(sentinel_data));
	helper.ConsumeFilters(&filter_set);

	HttpUrl url("https://host/svc/Entity");
	auto applied = helper.ApplyFiltersToUrl(url).ToString();
	REQUIRE(applied.find("$top") == std::string::npos);
}

TEST_CASE("$top is pushed when every filter reached the server", "[odata_filter]") {
	ODataPredicatePushdownHelper helper({"Col"});
	helper.SetODataVersion(ODataVersion::V4);
	helper.ConsumeLimit(10);

	duckdb::TableFilterSet filter_set;
	duckdb_compat::SetFilter(filter_set, 0, MakeEq(Value::INTEGER(1)));
	helper.ConsumeFilters(&filter_set);

	HttpUrl url("https://host/svc/Entity");
	auto applied = helper.ApplyFiltersToUrl(url).ToString();
	REQUIRE(applied.find("$top=10") != std::string::npos);
}
