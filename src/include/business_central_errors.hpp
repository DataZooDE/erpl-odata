#pragma once

#include <string>

namespace erpl_web {

// Business Central failure reporting (GitHub #277).
//
// A wrong ENVIRONMENT makes every Business Central request answer
// `404 <code>NoEnvironment</code>`; before this, bc_show_companies and bc_read turned that
// into "INTERNAL Error: Table function must return at least one column" and
// bc_show_entities into an empty result. These functions are pure (no duckdb, no I/O): they
// read the HTTP status and Business Central's error code out of the text of a failure and
// say what to check. The DuckDB side (RethrowWithBusinessCentralContext) throws the result.

struct BusinessCentralHttpFailure {
    int status = 0;        // 0: the text carries no "HTTP <status>"
    std::string code;      // Business Central's error code, e.g. "NoEnvironment"; may be empty
    std::string message;   // Business Central's error message; may be empty
};

// Finds the first "HTTP <3 digits>" and the error code/message of an OData error body in
// either form Business Central sends: XML (<code>..</code><message>..</message>) for
// $metadata, JSON ({"error": {"code": "..", "message": ".."}}) for data requests.
BusinessCentralHttpFailure ParseBusinessCentralHttpFailure(const std::string &error_text);

// Every message starts with this, so an already-described failure is not wrapped twice.
extern const char *const BUSINESS_CENTRAL_ERROR_PREFIX;

// A user-facing message naming the status, the environment (and company, if any) and what
// to check. `company` may be empty.
std::string DescribeBusinessCentralFailure(const BusinessCentralHttpFailure &failure,
                                           const std::string &environment,
                                           const std::string &company);

}  // namespace erpl_web
