#include "business_central_errors.hpp"

#include <regex>

namespace erpl_web {

const char *const BUSINESS_CENTRAL_ERROR_PREFIX = "Business Central ";

namespace {

std::string FirstMatch(const std::string &text, const std::regex &pattern) {
    std::smatch match;
    if (std::regex_search(text, match, pattern) && match.size() > 1) {
        return match[1].str();
    }
    return std::string();
}

}  // namespace

BusinessCentralHttpFailure ParseBusinessCentralHttpFailure(const std::string &error_text) {
    BusinessCentralHttpFailure failure;

    static const std::regex status_pattern(R"(HTTP ([1-5][0-9][0-9])\b)");
    auto status = FirstMatch(error_text, status_pattern);
    if (!status.empty()) {
        failure.status = std::stoi(status);
    }

    static const std::regex xml_code(R"(<code>([^<]*)</code>)");
    static const std::regex xml_message(R"(<message>([^<]*)</message>)");
    static const std::regex json_code(R"re("code"\s*:\s*"([^"]*)")re");
    static const std::regex json_message(R"re("message"\s*:\s*"([^"]*)")re");

    failure.code = FirstMatch(error_text, xml_code);
    if (failure.code.empty()) {
        failure.code = FirstMatch(error_text, json_code);
    }
    failure.message = FirstMatch(error_text, xml_message);
    if (failure.message.empty()) {
        failure.message = FirstMatch(error_text, json_message);
    }
    return failure;
}

std::string DescribeBusinessCentralFailure(const BusinessCentralHttpFailure &failure,
                                           const std::string &environment,
                                           const std::string &company) {
    std::string status = "HTTP " + std::to_string(failure.status);
    if (!failure.code.empty()) {
        status += ", " + failure.code;
        if (!failure.message.empty()) {
            status += ": " + failure.message;
        }
    }

    const std::string where = "environment '" + environment + "'" +
                              (company.empty() ? std::string() : ", company '" + company + "'");
    const std::string prefix = BUSINESS_CENTRAL_ERROR_PREFIX;

    if (failure.code == "NoEnvironment") {
        return prefix + "environment '" + environment + "' does not exist (" + status +
               "). Check the ENVIRONMENT of the business_central secret: it must be the exact "
               "name of an environment of this tenant (e.g. 'production' or 'sandbox'). The "
               "Business Central admin center lists the environments of a tenant.";
    }

    switch (failure.status) {
    case 401:
        return prefix + "rejected the credentials (" + status + ") for " + where +
               ". Check TENANT_ID, CLIENT_ID and CLIENT_SECRET of the business_central secret, "
               "and that the Microsoft Entra application is registered in Business Central "
               "(page 'Microsoft Entra Applications') with the API.ReadWrite.All application "
               "permission granted.";
    case 403:
        return prefix + "denied access (" + status + ") for " + where +
               ". The Microsoft Entra application is authenticated but lacks permission: "
               "assign it permission sets on the 'Microsoft Entra Applications' page in "
               "Business Central.";
    case 404:
        return prefix + "returned not found (" + status + ") for " + where +
               ". Check the company (bc_show_companies() lists them) and the entity name "
               "(bc_show_entities() lists them).";
    default:
        return prefix + "request failed (" + status + ") for " + where + ".";
    }
}

}  // namespace erpl_web
