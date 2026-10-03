#pragma once

#include "duckdb/main/secret/secret.hpp"

#include <map>
#include <string>

#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/named_parameter_map.hpp"
#include <vector>

namespace erpl_web 
{   
    class ArgBuilder {
    public:
        ArgBuilder();
        ArgBuilder& Add(const std::string& name, const duckdb::Value& value);
        ArgBuilder& Add(const std::string& name, duckdb::Value& value);
        ArgBuilder& Add(const std::string& name, const ArgBuilder& builder);
        ArgBuilder& Add(const std::string& name, const duckdb::vector<duckdb::Value>& values);
        ArgBuilder& Add(const std::string& name, const std::vector<duckdb::Value>& std_vector);
        ArgBuilder& Add(const std::string& name, const std::initializer_list<duckdb::Value>& values);
        duckdb::Value Build() const;
        std::vector<duckdb::Value> BuildArgList() const;
    private:
        duckdb::child_list_t<duckdb::Value> _args;
    };

    class ValueHelper {
    public:
        ValueHelper(duckdb::Value& value);
        ValueHelper(duckdb::Value& value, const std::vector<std::string>& root_path);
        ValueHelper(duckdb::Value& value, const std::string& root_path);
        
        duckdb::Value operator[](const std::string& name);
        std::vector<std::string> GetPathWithRoot(const std::string& path) const;
        static duckdb::Value GetValueForPath(duckdb::Value& value, const std::vector<std::string>& tokens);
        static duckdb::Value CreateMutatedValue(duckdb::Value& old_value, duckdb::Value& new_value, const std::string& path);
        static duckdb::Value CreateMutatedValue(duckdb::Value& old_value, duckdb::Value& new_value, const std::vector<std::string>& tokens);
        static duckdb::Value AddToList(duckdb::Value& current_list, duckdb::Value& new_value);
        static duckdb::Value RemoveFromList(duckdb::Value& current_list, duckdb::Value& remove_value);
        static duckdb::Value RemoveFromList(duckdb::Value& current_list, unsigned int index_to_remove);
        static std::vector<std::string> ParseJsonPointer(const std::string& path);
        static bool IsX(const duckdb::Value& value);
        static bool IsX(duckdb::Value& value);
        duckdb::Value& Get();
        void Print() const;
        void Print(const std::string& path) const;
    private:
        duckdb::Value& _value;
        std::vector<std::string> _root_path;
    };

    using named_parameter_map_t = duckdb::named_parameter_map_t;
    bool HasParam(const named_parameter_map_t& named_params, const std::string& name);
    duckdb::Value ConvertBoolArgument(const named_parameter_map_t& named_params, const std::string& name, const bool default_value);

    // Utility: extract list of strings from a DuckDB Value of LIST(VARCHAR)
    std::vector<std::string> GetStringList(const duckdb::Value &val);
    

// Converts a DuckDB MAP<VARCHAR, VARCHAR> argument into a plain map.
//
// Lived as a file-local helper in datasphere_read.cpp while sac_read_functions.cpp
// registered the same `params` argument and never read it - so `params => MAP{...}` was
// accepted and silently dropped by every SAC reader (GitHub #243). Shared so the two
// cannot drift, and so the next reader that takes `params` has one obvious thing to call.
std::map<std::string, std::string> ExtractInputParameters(const duckdb::Value &params_value,
                                                          const char *trace_component);


// Reads a required value out of a secret, or reports the missing key as USER input error.
//
// KeyValueSecret::TryGetValue(key, true) throws DuckDB's InternalException, which is
// reserved for invariant violations and INVALIDATES THE WHOLE DATABASE INSTANCE - so a
// user who left `tenant_name` out of a CREATE SECRET took the database down and got
// "INTERNAL Error: Failed to fetch key 'tenant_name' from secret" for their trouble. A
// missing key in a secret the user wrote is ordinary bad input. See GitHub #243.
std::string RequireSecretValue(const duckdb::KeyValueSecret &secret, const std::string &key,
                               const std::string &secret_name);

} // namespace erpl_web