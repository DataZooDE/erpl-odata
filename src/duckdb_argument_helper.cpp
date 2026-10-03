#include "duckdb_argument_helper.hpp"
#include "tracing.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

using namespace duckdb;

namespace erpl_web 
{   
    ArgBuilder::ArgBuilder() = default;

    ArgBuilder& ArgBuilder::Add(const std::string& name, const Value& value) 
    {
        _args.emplace_back(name, value);
        return *this;
    }

    ArgBuilder& ArgBuilder::Add(const std::string& name, Value& value) 
    {
        _args.emplace_back(name, value);
        return *this;
    }

    ArgBuilder& ArgBuilder::Add(const std::string& name, const ArgBuilder& builder) 
    {
        _args.emplace_back(name, builder.Build());
        return *this;
    }

    ArgBuilder& ArgBuilder::Add(const std::string& name, const duckdb::vector<Value>& values) 
    {
        if (values.empty()) {
            return Add(name, Value::LIST(duckdb::vector<Value>{}));
        }

        auto list_value = Value::LIST(values);
        return Add(name, list_value);
    }

    ArgBuilder& ArgBuilder::Add(const std::string& name, const std::vector<Value>& std_vector) 
    {
        duckdb::vector<Value> duck_vector;
        duck_vector.reserve(std_vector.size()); // Pre-allocate for efficiency
        
        for (const auto& v : std_vector) {
            duck_vector.push_back(v);
        }
        return Add(name, Value::LIST(duck_vector));
    }

    ArgBuilder& ArgBuilder::Add(const std::string& name, const std::initializer_list<Value>& values) 
    {
        auto list_value = Value::LIST(values);
        return Add(name, list_value);
    }

    Value ArgBuilder::Build() const {
        return Value::STRUCT(_args);
    }

    std::vector<Value> ArgBuilder::BuildArgList() const {
        return { Build() };
    }

    // ValueHelper ---------------------------------------------------------------

    ValueHelper::ValueHelper(Value& value) : _value(value) 
    { }

    ValueHelper::ValueHelper(Value& value, const std::vector<std::string>& root_path) 
        : _value(value), _root_path(root_path) 
    { }

    ValueHelper::ValueHelper(Value& value, const std::string& root_path) 
        : _value(value), _root_path(ParseJsonPointer(root_path)) 
    { }

    Value ValueHelper::operator[](const std::string& name) 
    {
        auto full_path = GetPathWithRoot(name);
        return GetValueForPath(_value, full_path);
    }

    std::vector<std::string> ValueHelper::GetPathWithRoot(const std::string& path) const
    {
        auto json_path = ParseJsonPointer(path);
    
        std::vector<std::string> full_path;
        full_path.reserve(_root_path.size() + json_path.size());       
        full_path.insert(full_path.end(), _root_path.begin(), _root_path.end());
        full_path.insert(full_path.end(), json_path.begin(), json_path.end());

        return full_path;
    }

    /**
     * Gets a value from a struct by JSON Path tokens
     * 
     * The functionality depends on whether the value is a struct or a list.
     *  - If the value is a struct, the first token is used to find the child value with the matching name.
     *  - If the value is a list, the first token is used to find the child value with the matching index.
     * 
     * @param value The struct value to get the value from.
     * @param tokens A vector of string tokens representing the keys or indices in the JSON document.
    */
    Value ValueHelper::GetValueForPath(Value& value, const std::vector<string>& tokens) 
    {
        if (tokens.empty()) {
            return value;
        }

        auto token_it = tokens.begin();
        auto remaining_tokens = std::vector<string>(token_it + 1, tokens.end());
        
        auto value_type = value.type();
        switch(value_type.id()) 
        {
            case LogicalTypeId::STRUCT:
            {
                auto child_values = StructValue::GetChildren(value);
                for (std::size_t i = 0; i < child_values.size(); i++) {
                    auto child_name = StructType::GetChildName(value_type, i);
                    if (child_name == *token_it) {
                        auto child_value = child_values[i];
                        
                        return GetValueForPath(child_value, remaining_tokens);
                    }
                }
                throw std::runtime_error(StringUtil::Format("Field '%s' not found in struct", *token_it));
            }
            case LogicalTypeId::LIST:
            {
                auto child_values = ListValue::GetChildren(value);
                std::size_t child_index;
                try {
                    child_index = std::stoul(*token_it);
                } catch (const std::exception&) {
                    throw std::runtime_error(StringUtil::Format("Invalid list index '%s'", *token_it));
                }
                
                if (child_index >= child_values.size()) {
                    throw std::runtime_error(StringUtil::Format("Index '%s' out of bounds for list", *token_it));
                }
                auto child_value = child_values[child_index];
                return GetValueForPath(child_value, remaining_tokens);
            }
                
            default:
                throw std::runtime_error(StringUtil::Format("Field '%s' is not a container type", *token_it));
        };
    }

    Value ValueHelper::CreateMutatedValue(Value& old_value, Value& new_value, const std::string& path) 
    {
        auto tokens = ParseJsonPointer(path);
        return CreateMutatedValue(old_value, new_value, tokens);
    }

    Value ValueHelper::CreateMutatedValue(Value& old_value, Value& new_value, const std::vector<std::string>& tokens) 
    {
        if (tokens.empty()) {
            return new_value;
        }

        auto token_it = tokens.begin();
        auto remaining_tokens = std::vector<string>(token_it + 1, tokens.end());

        auto value_type = old_value.type();
        switch(value_type.id()) 
        {
            case LogicalTypeId::STRUCT:
            {
                auto child_values = StructValue::GetChildren(old_value);
                auto new_child_pairs = child_list_t<Value>();

                for (std::size_t i = 0; i < child_values.size(); i++) {
                    auto child_name = StructType::GetChildName(value_type, i);
                    auto new_child_value = child_name == *token_it 
                                            ? CreateMutatedValue(child_values[i], new_value, remaining_tokens) 
                                            : child_values[i];
                    new_child_pairs.emplace_back(child_name, new_child_value);
                }
                return Value::STRUCT(new_child_pairs);
            }
            case LogicalTypeId::LIST:
            {
                auto child_values = ListValue::GetChildren(old_value);
                std::size_t child_index;
                try {
                    child_index = std::stoul(*token_it);
                } catch (const std::exception&) {
                    throw std::runtime_error(StringUtil::Format("Invalid list index '%s'", *token_it));
                }
                
                if (child_index >= child_values.size()) {
                    throw std::runtime_error(StringUtil::Format("Index '%s' out of bounds for list", *token_it));
                }
                child_values[child_index] = CreateMutatedValue(child_values[child_index], new_value, remaining_tokens);
                return Value::LIST(child_values);
            }
                
            default:
                throw std::runtime_error(StringUtil::Format("Field '%s' is not a container type", *token_it));
        };
    }

    Value ValueHelper::AddToList(Value& current_list, Value& new_value) 
    {
        if (current_list.type().id() != LogicalTypeId::LIST) {
            throw std::runtime_error("First argument is not a list");
        }

        auto list_values = ListValue::GetChildren(current_list);
        if (!list_values.empty()) {
            auto current_type = list_values[0].type();
            auto casted = new_value.DefaultCastAs(current_type);
            list_values.push_back(casted);
        } else {
            list_values.push_back(new_value);
        }

        return Value::LIST(list_values);
    }

    Value ValueHelper::RemoveFromList(Value& current_list, Value& remove_value) 
    {
        if (current_list.type().id() != LogicalTypeId::LIST) {
            throw std::runtime_error("First argument is not a list");
        }

        auto child_values = ListValue::GetChildren(current_list);
        auto new_list_values = duckdb::vector<Value>();
        new_list_values.reserve(child_values.size()); // Pre-allocate for efficiency

        for (const auto& v : child_values) {
            if (v != remove_value) {
                new_list_values.push_back(v);
            }
        }

        return Value::LIST(remove_value.type(), new_list_values);
    }

    Value ValueHelper::RemoveFromList(Value& current_list, unsigned int index_to_remove) 
    {
        if (current_list.type().id() != LogicalTypeId::LIST) {
            throw std::runtime_error("First argument is not a list");
        }

        auto child_values = ListValue::GetChildren(current_list);
        if (index_to_remove >= child_values.size()) {
            throw std::runtime_error(StringUtil::Format("Index %u out of bounds for list", index_to_remove));
        }
        
        auto new_list_values = duckdb::vector<Value>();
        new_list_values.reserve(child_values.size() - 1); // Pre-allocate for efficiency

        for (std::size_t i = 0; i < child_values.size(); i++) {
            if (i != index_to_remove) {
                new_list_values.push_back(child_values[i]);
            }
        }

        return Value::LIST(new_list_values);
    }

    /**
     * Parses a JSON Pointer string into a vector of string tokens.
     *
     * A JSON Pointer is a string syntax for identifying a specific value within a JSON document.
     * This function takes a JSON Pointer string and returns a vector of string tokens, where each token
     * represents a key or index in the JSON document.
     *
     * @param path The JSON Pointer string to parse.
     * @return A vector of string tokens representing the keys or indices in the JSON document.
     *
     * @example
     * std::string path = "/foo/0/bar";
     * std::vector<std::string> tokens = ParseJsonPointer(path);
     * // tokens = {"foo", "0", "bar"}
     *
     * @example
     * std::string path = "/a~1b/c~0d";
     * std::vector<std::string> tokens = ParseJsonPointer(path);
     * // tokens = {"a/b", "c~d"}
     */
    std::vector<std::string> ValueHelper::ParseJsonPointer(const std::string& path) 
    {
        std::vector<std::string> tokens;
        tokens.reserve(std::count(path.begin(), path.end(), '/') + 1); // Pre-allocate for efficiency
        
        std::string token;
        std::istringstream token_stream(path);

        while (std::getline(token_stream, token, '/')) {
            // Unescape ~1 and ~0 sequences.
            size_t pos = 0;
            while ((pos = token.find("~1", pos)) != std::string::npos) {
                token.replace(pos, 2, "/");
                pos += 1;
            }
            pos = 0;
            while ((pos = token.find("~0", pos)) != std::string::npos) {
                token.replace(pos, 2, "~");
                pos += 1;
            }

            if (!token.empty()) {
                tokens.push_back(token);
            }
        }

        return tokens;
    }

    bool ValueHelper::IsX(const Value& value) {
        return value.ToString() == "X";
    }

    bool ValueHelper::IsX(Value& value) {
        return value.ToString() == "X";
    }

    Value& ValueHelper::Get()
    {
        return _value;
    }

    void ValueHelper::Print() const {
        _value.Print();
    }

    void ValueHelper::Print(const std::string& path) const {
        auto full_path = GetPathWithRoot(path);
        auto val = GetValueForPath(_value, full_path);
        val.Print();
    }

    // ValueHelper ---------------------------------------------------------------

    bool HasParam(const named_parameter_map_t& named_params, const std::string& name) {
        return named_params.find(name) != named_params.end();
    };

    Value ConvertBoolArgument(const named_parameter_map_t& named_params, const std::string& name, const bool default_value) {
        auto value = HasParam(named_params, name) ? BooleanValue::Get(named_params.at(name)) : default_value;

        if (value == true) {
            return Value::CreateValue("X");
        } else {
            return Value::CreateValue("");
        }
    }
    
    std::vector<std::string> GetStringList(const Value &val) {
        std::vector<std::string> out;
        if (val.IsNull()) {
            return out;
        }
        if (val.type().id() != LogicalTypeId::LIST) {
            return out;
        }
        auto children = ListValue::GetChildren(val);
        out.reserve(children.size());
        for (auto &c : children) {
            if (!c.IsNull() && c.type().id() == LogicalTypeId::VARCHAR) {
                out.emplace_back(StringValue::Get(c));
            }
        }
        return out;
    }

} // namespace erpl_web
namespace erpl_web {

std::map<std::string, std::string> ExtractInputParameters(const duckdb::Value &params_value,
                                                          const char *trace_component) {
    std::map<std::string, std::string> input_params;

    if (params_value.type().id() != duckdb::LogicalTypeId::MAP) {
        ERPL_TRACE_ERROR(trace_component, "Params parameter must be a MAP<VARCHAR, VARCHAR> type");
        return input_params;
    }

    auto map_entries = duckdb::MapValue::GetChildren(params_value);
    ERPL_TRACE_DEBUG(trace_component,
                     "Processing " + std::to_string(map_entries.size()) + " input parameters");

    // DuckDB MAPs are stored as a list of structs with 'key' and 'value' fields
    for (const auto &entry : map_entries) {
        if (entry.type().id() != duckdb::LogicalTypeId::STRUCT) {
            continue;
        }
        auto struct_entries = duckdb::StructValue::GetChildren(entry);
        auto struct_types = duckdb::StructType::GetChildTypes(entry.type());

        std::string key, value;
        for (size_t j = 0; j < struct_types.size() && j < struct_entries.size(); j++) {
            if (struct_types[j].first == "key") {
                key = struct_entries[j].ToString();
            } else if (struct_types[j].first == "value") {
                value = struct_entries[j].ToString();
            }
        }

        if (!key.empty() && !value.empty()) {
            input_params[key] = value;
            ERPL_TRACE_DEBUG(trace_component, "Added input parameter: " + key + " = " + value);
        }
    }

    return input_params;
}

} // namespace erpl_web

namespace erpl_web {

std::string RequireSecretValue(const duckdb::KeyValueSecret &secret, const std::string &key,
                               const std::string &secret_name) {
    auto value = secret.secret_map.find(key);
    if (value == secret.secret_map.end() || value->second.IsNull()) {
        throw duckdb::InvalidInputException(
            "Secret '%s' is missing the required field '%s'. Add it to the CREATE SECRET "
            "statement that defines this secret.", secret_name.c_str(), key.c_str());
    }
    return value->second.ToString();
}

std::shared_ptr<HttpAuthParams> ResolveHttpAuthFromSecrets(duckdb::ClientContext &context, const std::string &url) {
    auto auth_params = std::make_shared<HttpAuthParams>();

    auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
    auto &secret_manager = duckdb::SecretManager::Get(context);

    auto basic_match = secret_manager.LookupSecret(transaction, url, "http_basic");
    if (basic_match.HasMatch()) {
        const auto &secret = dynamic_cast<const duckdb::KeyValueSecret &>(basic_match.GetSecret());
        const auto secret_name = secret.GetName();
        auth_params->basic_credentials = std::make_tuple(RequireSecretValue(secret, "username", secret_name),
                                                         RequireSecretValue(secret, "password", secret_name));
        return auth_params;
    }

    auto bearer_match = secret_manager.LookupSecret(transaction, url, "http_bearer");
    if (bearer_match.HasMatch()) {
        const auto &secret = dynamic_cast<const duckdb::KeyValueSecret &>(bearer_match.GetSecret());
        auth_params->bearer_token = RequireSecretValue(secret, "token", secret.GetName());
    }

    return auth_params;
}

} // namespace erpl_web
