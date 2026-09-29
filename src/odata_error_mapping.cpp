#include "duckdb/function/table_function.hpp"

#include "datazoo/oauth2/http_client.hpp"
#include "odata_edm.hpp"
#include "odata_expand_parser.hpp"
#include "odata_read_functions.hpp"
#include "odata_url_helpers.hpp"
#include "yyjson.hpp"

#include <unordered_set>

#include "tracing.hpp"
#include "telemetry.hpp"
#include "erpl_odata_banner.hpp"

// Mapping of transport/runtime failures onto user-facing DuckDB exceptions.
// Shared by the OData, ODP and Datasphere read paths.
//
// Declarations live in src/include/odata_read_functions.hpp.

namespace erpl_web {

// ============================================================================
// Shared Error Handling Utilities
// ============================================================================

namespace ODataErrorHandling {

/**
 * @brief Convert runtime errors (especially HTTP errors) to user-friendly InvalidInputException
 * 
 * @param e The runtime error to convert
 * @param url The URL that caused the error
 * @param service_type Type of service ("OData" or "ODP OData") for error messages
 * @param discovery_function Name of discovery function to suggest (e.g., "sap_odata_show()" or "sap_odp_odata_show()")
 * @return InvalidInputException with user-friendly error message
 */
duckdb::InvalidInputException ConvertHttpErrorToUserFriendly(const std::runtime_error& e, 
                                                           const std::string& url,
                                                           const std::string& service_type,
                                                           const std::string& discovery_function) {
    std::string error_msg = e.what();
    
    if (error_msg.find("HTTP 404") != std::string::npos) {
        return duckdb::InvalidInputException(service_type + " service not found at URL: " + url + 
            ". Please check if the URL path is correct, especially the entity set name. " +
            "Use " + discovery_function + " to discover available entity sets.");
    } else if (error_msg.find("HTTP 401") != std::string::npos) {
        return duckdb::InvalidInputException("Authentication failed for " + service_type + " service at: " + url + 
            ". Please check your credentials in the secret.");
    } else if (error_msg.find("HTTP 403") != std::string::npos) {
        return duckdb::InvalidInputException("Access forbidden to " + service_type + " service at: " + url + 
            ". Please check if your user has permission to access this service.");
    } else if (error_msg.find("Connection failed") != std::string::npos) {
        return duckdb::InvalidInputException("Failed to connect to " + service_type + " service at: " + url + 
            ". Please check if the server is running and accessible.");
    } else {
        // Re-throw with additional context
        return duckdb::InvalidInputException("Failed to access " + service_type + " service at: " + url + 
            ". Error: " + error_msg);
    }
}

} // namespace ODataErrorHandling

} // namespace erpl_web
