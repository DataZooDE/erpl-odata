#include "duckdb/common/types/string_type.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include "odata_attach_functions.hpp"
#include "telemetry.hpp"
#include "erpl_odata_banner.hpp"

namespace erpl_web {

using duckdb::PostHogTelemetry;

duckdb::unique_ptr<ODataAttachBindData> ODataAttachBindData::FromUrl(const std::string& url, 
                                                                     std::shared_ptr<HttpAuthParams> auth_params)
{
    auto http_client = std::make_shared<HttpClient>();
    auto odata_client = std::make_shared<ODataServiceClient>(http_client, url, auth_params);

    return duckdb::make_uniq<ODataAttachBindData>(odata_client);
}

ODataAttachBindData::ODataAttachBindData(std::shared_ptr<ODataServiceClient> odata_client)
    : odata_client(odata_client)
{ }


bool ODataAttachBindData::IsFinished() const
{
    return finished;
}

void ODataAttachBindData::SetFinished()
{
    finished = true;
}

std::vector<std::string> ODataAttachBindData::IgnorePatterns() const
{
    return ignore_patterns;
}

void ODataAttachBindData::IgnorePatterns(const std::vector<std::string> &ignore)
{
    this->ignore_patterns = ignore;
}

bool ODataAttachBindData::Overwrite() const
{
    return overwrite;
}

void ODataAttachBindData::SetOverwrite(bool overwrite)
{
    this->overwrite = overwrite;
}

std::vector<ODataEntitySetReference> ODataAttachBindData::EntitySets()
{
    auto svc_response = odata_client->Get();
    auto svc_references = svc_response->EntitySets();

    auto filtered_svc_references = std::vector<ODataEntitySetReference>();
    for (auto &svc_reference : svc_references) {
        if (MatchPattern(svc_reference.name, ignore_patterns)) {
            continue;
        }

        svc_reference.MergeWithBaseUrlIfRelative(odata_client->Url());
        filtered_svc_references.push_back(svc_reference);   
    }

    return filtered_svc_references;
}

bool ODataAttachBindData::MatchPattern(const std::string &str, const std::string &ignore_pattern)
{
    auto is_match = duckdb::Glob(str.c_str(), str.size(), ignore_pattern.c_str(), ignore_pattern.size());
    return is_match;
}

bool ODataAttachBindData::MatchPattern(const std::string &str, const std::vector<std::string> &ignore_patterns)
{
    for (auto &ignore_pattern : ignore_patterns) {
        if (MatchPattern(str, ignore_pattern)) {
            return true;
        }
    }

    return false;
}

// -------------------------------------------------------------------------------------------------

static std::shared_ptr<HttpAuthParams> AuthParamsFromInput(duckdb::ClientContext &context, TableFunctionBindInput &input)
{
    auto args = input.inputs;
    auto url = args[0].ToString();
    return HttpAuthParams::FromDuckDbSecrets(context, url);
}

static std::vector<std::string> ParseIgnoreParameter(duckdb::Value &ignore)
{
    std::vector<std::string> ret;

    auto ignore_type = ignore.type().id();
    if (ignore_type == LogicalTypeId::LIST) {
        auto ignore_list = ListValue::GetChildren(ignore);
        auto ignore_values = std::vector<std::string>();
        for (auto &ignore_value : ignore_list) { 
            ret.push_back(ignore_value.ToString());
        }
    } else {
        ret.push_back(ignore.ToString());
    }

    return ret;
}

static void ParseNamedParameters(TableFunctionBindInput &input, duckdb::unique_ptr<ODataAttachBindData> &bind_data)
{
    for (auto &kv : input.named_parameters) {
		if (kv.first == "overwrite") {
			bind_data->SetOverwrite(BooleanValue::Get(kv.second));
		}

        if (kv.first == "ignore") {
            bind_data->IgnorePatterns(ParseIgnoreParameter(kv.second));
        }
	}
}

static unique_ptr<FunctionData> ODataAttachBind(ClientContext &context,
                                               TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types,
                                               vector<string> &names)
{
    PostHogTelemetry::Instance().RecordFunctionCall("odata_attach");
    auto auth_params = AuthParamsFromInput(context, input);
    auto url = input.inputs[0].GetValue<std::string>();
    auto bind_data = ODataAttachBindData::FromUrl(url, auth_params);
    ParseNamedParameters(input, bind_data);

    return_types.emplace_back(LogicalTypeId::BOOLEAN);
    names.emplace_back("Success");

    return std::move(bind_data);
}

static void ODataAttachScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) 
{
    // One-shot by design, and the latch stays on the BIND DATA on purpose. This scan is a
    // side-effecting DDL statement: it fetches the service document and creates a view per
    // entity set with replace = Overwrite(), which defaults to false. Giving it
    // per-execution state would make a second EXECUTE of a bound plan re-run the DDL and
    // fail with "already exists" where it previously did nothing. See scan_row_cursor.hpp
    // and the exemption list in test_graph_scan_reexecution.cpp.
    auto &data = data_p.bind_data->CastNoConst<ODataAttachBindData>();
	if (data.IsFinished()) {
		output.SetCardinality(0);
		return;
	}

    auto duck_conn = duckdb::Connection(context.db->GetDatabase(context));
    
    auto svc_references = data.EntitySets();
    for (auto &svc_reference : svc_references) {
        auto table_relation = duck_conn.TableFunction("odata_read", {Value(svc_reference.url)});
        auto table_view = table_relation->CreateView(svc_reference.name, data.Overwrite(), false);
    }
	
    data.SetFinished();
    output.SetCardinality(0);
}

TableFunctionSet CreateODataAttachFunction()
{
    TableFunctionSet function_set("odata_attach");

    TableFunction attach_service_ignore_complex({LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataAttachScan), DATAZOO_GUARD(ERPL_ODATA_BANNER, ODataAttachBind));
    attach_service_ignore_complex.named_parameters["overwrite"] = LogicalTypeId::BOOLEAN;
    attach_service_ignore_complex.named_parameters["ignore"] = LogicalType::LIST(LogicalTypeId::VARCHAR);
    function_set.AddFunction(attach_service_ignore_complex);

    return function_set;
}


} // namespace erpl_web