#include <limits>
#include "duckdb_compat.hpp"
#include <cpptrace/cpptrace.hpp>

#include "odata_client.hpp"
#include "tracing.hpp"
#include "odata_url_helpers.hpp"

namespace erpl_web {

uint32_t ValidateMaxPageSizeParameter(const duckdb::Value& value)
{
    // NULL reaches GetValue<uint64_t>() as 0 on some paths and throws on others; either
    // way "max_page_size => NULL" is a caller mistake that deserves its own message rather
    // than the reject-zero one, which would misdescribe what they wrote (GitHub #185).
    if (value.IsNull()) {
        throw duckdb::InvalidInputException(
            "max_page_size must be a positive integer, not NULL; omit the parameter to send "
            "no page-size preference at all");
    }
    const auto requested = value.GetValue<uint64_t>();
    if (requested == 0) {
        throw duckdb::InvalidInputException(
            "max_page_size must be greater than 0; omit the parameter to send no page-size "
            "preference at all");
    }
    if (requested > std::numeric_limits<uint32_t>::max()) {
        throw duckdb::InvalidInputException(
            "max_page_size must fit in 32 bits; %llu is larger than any service will honour",
            static_cast<unsigned long long>(requested));
    }
    return static_cast<uint32_t>(requested);
}

std::string BuildPageLimitExceededMessage(idx_t limit, const std::string& last_url)
{
    return "OData server-driven paging exceeded the limit of " + std::to_string(limit) +
           " pages; the service keeps advertising a next link. Either the service is "
           "looping, or this extraction genuinely needs more pages than the limit allows. "
           "In that case narrow the read with a filter, or ask the service for larger pages "
           "with the max_page_size parameter (odata_read(..., max_page_size => 5000)), which "
           "sends Prefer: odata.maxpagesize. Last URL: " + last_url;
}



// ----------------------------------------------------------------------

ODataEntitySetResponse::ODataEntitySetResponse(std::unique_ptr<HttpResponse> http_response, ODataVersion odata_version)
    : ODataResponse(std::move(http_response))
    , odata_version(odata_version)
{ 
    ERPL_TRACE_DEBUG("ODATA_RESPONSE", "Created OData entity set response");
    ERPL_TRACE_DEBUG("ODATA_RESPONSE", "Response content type: " + ContentType());
    ERPL_TRACE_DEBUG("ODATA_RESPONSE", std::string("OData version: ") + (odata_version == ODataVersion::V2 ? "V2" : "V4"));
}

std::string ODataEntitySetResponse::MetadataContextUrl()
{
    return Content()->MetadataContextUrl();
}

std::optional<std::string> ODataEntitySetResponse::NextUrl()
{
    return Content()->NextUrl();
}

std::vector<std::vector<duckdb::Value>> ODataEntitySetResponse::ToRows(std::vector<std::string> &column_names, 
                                                             std::vector<duckdb::LogicalType> &column_types)
{
    return Content()->ToRows(column_names, column_types);
}

// ----------------------------------------------------------------------

// ODataClient base class implementation is in erpl_odata_client.hpp

template<typename TResponse>
void ODataClient<TResponse>::DetectODataVersion()
{
    // If we already know the version, don't fetch metadata again
    if (odata_version != ODataVersion::UNKNOWN) {
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "OData version already detected, skipping metadata fetch");
        return;
    }
    
    // For Datasphere, prefer using @odata.context from prior responses; do not skip metadata
    
    // Get the current metadata to detect version
    auto metadata_url = GetMetadataContextUrl();
    
    // Check if we already have cached metadata for this URL
    auto cached_edmx = EdmCache::GetInstance().Get(metadata_url);
    if (cached_edmx) {
        // Version is already detected from the cached metadata
        // Extract the version from cached metadata and update client version
        odata_version = cached_edmx->GetVersion();
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Using cached metadata, detected version: " + std::string(odata_version == ODataVersion::V2 ? "V2" : "V4"));
        return;
    }

    ERPL_TRACE_INFO("ODATA_CLIENT", "Fetching metadata to detect OData version from: " + metadata_url);
    
    try {
        // Fetch metadata to detect version
        auto metadata_response = DoMetadataHttpGet(metadata_url);
        if (!metadata_response) {
            ERPL_TRACE_WARN("ODATA_CLIENT", "Failed to get metadata response, will try to detect from data response");
            return;
        }
        
        auto content = metadata_response->Content();
        if (content.empty()) {
            ERPL_TRACE_WARN("ODATA_CLIENT", "Empty metadata content, will try to detect from data response");
            return;
        }
        
        // Parse metadata to detect version
        auto edmx = Edmx::FromXml(content);
        odata_version = edmx.GetVersion();
        
        ERPL_TRACE_INFO("ODATA_CLIENT", "Detected OData version: " + std::string(odata_version == ODataVersion::V2 ? "V2" : "V4"));
        
        // Cache the metadata with detected version
        EdmCache::GetInstance().Set(metadata_url, edmx);
        
    } catch (const std::exception& e) {
        ERPL_TRACE_WARN("ODATA_CLIENT", "Failed to fetch or parse metadata: " + std::string(e.what()) + ", will try to detect from data response");
        // Don't throw here - we'll try to detect version from the actual data response
    }
}

// ----------------------------------------------------------------------

ODataEntitySetClient::ODataEntitySetClient(std::shared_ptr<HttpClient> http_client, const HttpUrl& url, const Edmx& edmx)
    : ODataClient(http_client, url, nullptr)
{ }

ODataEntitySetClient::ODataEntitySetClient(std::shared_ptr<HttpClient> http_client, const HttpUrl& url)
    : ODataClient(http_client, url, nullptr)
{ }

ODataEntitySetClient::ODataEntitySetClient(std::shared_ptr<HttpClient> http_client, const HttpUrl& url, const Edmx& edmx, std::shared_ptr<HttpAuthParams> auth_params)
    : ODataClient(http_client, url, auth_params)
{ }

ODataEntitySetClient::ODataEntitySetClient(std::shared_ptr<HttpClient> http_client, const HttpUrl& url, std::shared_ptr<HttpAuthParams> auth_params)
    : ODataClient(http_client, url, auth_params)
{ }

std::string ODataEntitySetClient::GetMetadataContextUrl()
{
    // Input parameters can change what resolveMetadataUrl() would produce, so the MEMOISED
    // derivation below is invalidated when they are present. An authoritative value - the
    // @odata.context the service returned, stored at bind time - is not a cache and is not
    // invalidated by them: it is what the service said its metadata lives at, regardless of
    // which parameters the query passes.
    //
    // Clearing both was why the #186 carry was a no-op for exactly the parameterized
    // Datasphere reads it was written for (GitHub #190).
    if (!input_parameters.empty() && !metadata_context_url_is_authoritative) {
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Input parameters present, clearing derived metadata URL");
        metadata_context_url.clear();
    }

    if (!metadata_context_url.empty()) {
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Using stored metadata context URL: " + metadata_context_url);
        return metadata_context_url;
    }

    std::string ctx;
    if (current_response) {
        ctx = current_response->MetadataContextUrl();
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Found @odata.context: " + ctx);
    }

    ODataUrlResolver resolver;
    auto final_url = resolver.resolveMetadataUrl(url, ctx);
    if (metadata_context_url != final_url) {
        ERPL_TRACE_INFO("ODATA_CLIENT", "Resolved metadata URL: " + final_url);
        metadata_context_url = final_url;
        // Derived, not authoritative: this one IS a cache.
        metadata_context_url_is_authoritative = false;
    }
    return metadata_context_url;
}
ODataEntitySetClient::ODataEntitySetClient(std::shared_ptr<HttpClient> http_client, const HttpUrl& url,
                                           const HttpUrl& service_origin, std::shared_ptr<HttpAuthParams> auth_params)
    : ODataClient(http_client, url, service_origin, auth_params)
{ }


std::shared_ptr<ODataEntitySetContent> ODataEntitySetResponse::CreateODataContent(const std::string& content, ODataVersion odata_version)
{
    ERPL_TRACE_DEBUG("ODATA_CONTENT", "Creating OData content from response");
    ERPL_TRACE_DEBUG("ODATA_CONTENT", "Content type: " + ContentType());
    ERPL_TRACE_DEBUG("ODATA_CONTENT", "Content size: " + std::to_string(content.length()) + " bytes");
    
    if (ODataJsonContentMixin::IsJsonContentType(ContentType())) {
        // Detection order (GitHub #77): the payload first, because what this decides is how to
        // PARSE the body and the body is ground truth for that. The declared protocol version
        // does not determine the JSON shape - OData v3 announces "DataServiceVersion: 3.0" for
        // both the verbose format, which wraps rows in "d", and the minimalmetadata format,
        // which uses a "value" array like v4. Then the headers, which are what actually helps
        // when the body says nothing (empty, non-JSON, or an error document). Then the version
        // the metadata document implied, carried by `odata_version`. Guessing V4 is the last
        // resort, not the first.
        auto detected_version = ODataJsonContentMixin::DetectODataVersionFromPayload(content);
        if (detected_version == ODataVersion::UNKNOWN) {
            detected_version = ODataJsonContentMixin::DetectODataVersionFromHeaders(http_response->headers);
        }
        if (detected_version == ODataVersion::UNKNOWN) {
            detected_version = (odata_version != ODataVersion::UNKNOWN) ? odata_version : ODataVersion::V4;
        }
        ERPL_TRACE_DEBUG("ODATA_CONTENT", std::string("Resolved OData version for response: ") + (detected_version == ODataVersion::V2 ? "V2" : "V4"));
        auto content_obj = std::make_shared<ODataEntitySetJsonContent>(content);
        content_obj->SetODataVersion(detected_version);
        return content_obj;
    }
        
    ERPL_TRACE_ERROR("ODATA_CONTENT", "Unsupported content type: " + ContentType());
    throw std::runtime_error("Unsupported OData content type: " + ContentType());
}

std::shared_ptr<ODataEntitySetResponse> ODataEntitySetClient::Get(bool get_next)
{
    if (! get_next && current_response != nullptr) {
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Returning cached response");
        return current_response;
    }

    ERPL_TRACE_INFO("ODATA_CLIENT", "Fetching OData request from: " + url.ToString() + " (get_next: " + std::to_string(get_next) + ")");

    // Ensure OData version is detected before making any requests
    if (odata_version == ODataVersion::UNKNOWN) {
        DetectODataVersion();
    }

    if (get_next && current_response == nullptr) {
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "get_next=true with no prior response — nothing to advance from");
        return nullptr;
    }

    if (get_next && current_response != nullptr) {
        auto next_url = current_response->NextUrl();
        if (!next_url) {
            ERPL_TRACE_DEBUG("ODATA_CLIENT", "No next URL available for pagination");
            return nullptr;
        }

        // A next link that resolves back onto the URL we just fetched would make
        // this loop issue the identical request forever, uninterruptibly. Stop
        // with an error instead: neither terminating nor reporting is worse than
        // failing. See GitHub #78.
        // The next link is chosen by the SERVICE and goes into the request line, which is
        // written verbatim on this path - every OData request sets url_encode = false, which
        // installs the verbatim target writer, so httplib's own escaping does not apply. A
        // link carrying CR/LF keeps its host (HttpUrl's parser matches both inside the path
        // and query), passes the same-origin check, and would inject a header onto a request
        // carrying the caller's credentials.
        if (!IsWireSafeUrl(next_url.value())) {
            throw duckdb::IOException(
                "OData service returned a next link containing characters that cannot be sent "
                "in a request.");
        }

        const auto previous_url = url.ToString();
        auto resolved_next_url = HttpUrl::MergeWithBaseUrlIfRelative(url, next_url.value());
        if (resolved_next_url.ToString() == previous_url) {
            throw std::runtime_error(
                "OData service returned a next link identical to the request that produced it, which would page "
                "forever. Repeated URL: " + previous_url);
        }

        if (page_requests >= MAX_PAGE_REQUESTS) {
            // Say what to do about it. A caller who hits this on a legitimately huge
            // extraction has no way to guess that a narrower query or a larger server page
            // size is the answer, and a bare "limit exceeded" reads like a bug in us.
            throw std::runtime_error(
                BuildPageLimitExceededMessage(MAX_PAGE_REQUESTS, resolved_next_url.ToString()));
        }
        page_requests++;

        url = resolved_next_url;
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Using next URL (page " + std::to_string(page_requests) + "): " + url.ToString());
    }

    // Add input parameters to the URL if they exist
    HttpUrl request_url = url;
    if (!input_parameters.empty()) {
        request_url = AddInputParametersToUrl(url);
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Modified URL with input parameters: " + request_url.ToString());
    }

    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Executing HTTP GET request");
    auto http_response = DoHttpGet(request_url);
    
    if (!http_response) {
        ERPL_TRACE_ERROR("ODATA_CLIENT", "Failed to get HTTP response");
        return nullptr;
    }
    
    // Detect OData version from raw HTTP response content if not already known
    if (odata_version == ODataVersion::UNKNOWN) {
        auto content_str = http_response->Content();
        if (ODataJsonContentMixin::IsJsonContentType(http_response->ContentType())) {
            auto detected_version = ODataJsonContentMixin::DetectODataVersion(content_str, http_response->headers);
            odata_version = detected_version;
            std::string version_str = (odata_version == ODataVersion::V2 ? "V2" : "V4");
            ERPL_TRACE_INFO("ODATA_CLIENT", "Detected OData version from response: " + version_str);
        } else {
            ERPL_TRACE_WARN("ODATA_CLIENT", "Non-JSON content type, cannot detect OData version from response");
        }
    }
    
    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Creating OData response object");
    current_response = std::make_shared<ODataEntitySetResponse>(std::move(http_response), odata_version);
    
    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Successfully created OData response");
    
    // After getting a response, try to extract and store the metadata context URL
    // This will be used for future metadata requests instead of generating fallback URLs
    if (current_response) {
        auto ctx = current_response->MetadataContextUrl();
        if (!ctx.empty()) {
            ERPL_TRACE_DEBUG("ODATA_CLIENT", "Raw metadata context URL: " + ctx);
            
            // Parse the fragment to extract entity information
            auto hash_pos = ctx.find('#');
            if (hash_pos != std::string::npos) {
                auto fragment = ctx.substr(hash_pos + 1);
                ERPL_TRACE_DEBUG("ODATA_CLIENT", "Metadata context fragment: " + fragment);
                
                // Extract entity name from fragment. Supported forms:
                //  - "Entity(params)/Set"
                //  - "Entity/Set"
                //  - "Entity"
                std::string entity_name;
                auto open_paren_pos = fragment.find('(');
                if (open_paren_pos != std::string::npos) {
                    entity_name = fragment.substr(0, open_paren_pos);
                } else {
                    auto slash_pos = fragment.find('/');
                    if (slash_pos != std::string::npos) {
                        entity_name = fragment.substr(0, slash_pos);
                    } else {
                        entity_name = fragment; // plain entity name
                    }
                }
                if (!entity_name.empty()) {
                    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Extracted entity name from fragment: " + entity_name);
                    current_entity_name_from_fragment = entity_name;
                }
                
                // Strip the fragment for the metadata URL
                ctx = ctx.substr(0, hash_pos);
            }
            
            // Strip any query from @odata.context-derived metadata URLs
            if (!ctx.empty()) {
                auto qpos = ctx.find('?');
                if (qpos != std::string::npos) {
                    ctx = ctx.substr(0, qpos);
                }
            }
            HttpUrl meta_url = HttpUrl::MergeWithBaseUrlIfRelative(url, ctx);
            // If @odata.context pointed to an entity-set-local $metadata like .../ServiceGroups/$metadata,
            // normalize to the service-root $metadata one level above the entity set: .../0002/$metadata
            {
                auto path_ctx = meta_url.Path();
                auto mpos = path_ctx.rfind("/$metadata");
                if (mpos != std::string::npos) {
                    // Compute service root from the current request URL path
                    auto req_path = url.Path();
                    auto last_slash = req_path.find_last_of('/');
                    if (last_slash != std::string::npos && last_slash > 0) {
                        std::string service_root = req_path.substr(0, last_slash);
                        meta_url.Path(service_root + "/$metadata");
                        meta_url.Query("");
                    }
                }
            }
            auto final_url = meta_url.ToString();
            if (metadata_context_url != final_url) {
                ERPL_TRACE_INFO("ODATA_CLIENT", "Updated metadata context URL from response: " + final_url);
                metadata_context_url = final_url;
            }
        } else if (!input_parameters.empty()) {
            // When input parameters are used but no metadata context URL is provided,
            // extract the entity name from the URL path since we know the structure
            ERPL_TRACE_DEBUG("ODATA_CLIENT", "No metadata context URL, extracting entity name from URL path with input parameters");
            
            auto path = url.Path();
            if (!path.empty() && path != "/") {
                // Remove leading slash if present
                if (path[0] == '/') {
                    path = path.substr(1);
                }
                
                // Find the last path segment
                auto last_slash_pos = path.find_last_of('/');
                if (last_slash_pos != std::string::npos) {
                    auto last_segment = path.substr(last_slash_pos + 1);
                    
                    // Extract entity name from segment like "flights_view(CARRIER='AA')/Set"
                    auto open_paren_pos = last_segment.find('(');
                    if (open_paren_pos != std::string::npos) {
                        auto entity_name = last_segment.substr(0, open_paren_pos);
                        current_entity_name_from_fragment = entity_name;
                        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Extracted entity name from URL path with input parameters: " + entity_name);
                    }
                }
            }
        }
    }
    
    return current_response;
}

void ODataEntitySetClient::SetEntitySetNameFromContextFragment(const std::string &context_or_fragment)
{
    // Only extract from a fragment part after '#'. If none, do nothing.
    auto hash_pos = context_or_fragment.find('#');
    if (hash_pos == std::string::npos) {
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "No fragment present in @odata.context; skipping entity name extraction");
        return;
    }
    std::string fragment = context_or_fragment.substr(hash_pos + 1);
    // Extract entity name as in Get(): before '(' or before '/' or whole fragment
    std::string entity_name;
    auto open_paren_pos = fragment.find('(');
    if (open_paren_pos != std::string::npos) {
        entity_name = fragment.substr(0, open_paren_pos);
    } else {
        auto slash_pos = fragment.find('/');
        if (slash_pos != std::string::npos) {
            entity_name = fragment.substr(0, slash_pos);
        } else {
            entity_name = fragment;
        }
    }
    if (!entity_name.empty()) {
        ERPL_TRACE_INFO("ODATA_CLIENT", "Set entity set name from @odata.context: " + entity_name);
        current_entity_name_from_fragment = entity_name;
    } else {
        ERPL_TRACE_WARN("ODATA_CLIENT", "Failed to extract entity name from @odata.context fragment: " + context_or_fragment);
    }
}

EntitySet ODataEntitySetClient::GetCurrentEntitySetType()
{
    ERPL_TRACE_DEBUG("ODATA_CLIENT", "GetCurrentEntitySetType called");
    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Current URL path: " + url.Path());
    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Input parameters count: " + std::to_string(input_parameters.size()));
    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Current entity name from fragment: " + (current_entity_name_from_fragment.empty() ? "empty" : current_entity_name_from_fragment));
    
    std::string entity_set_name;
    
    // First, try to use the entity name extracted from the metadata context fragment
    if (!current_entity_name_from_fragment.empty()) {
        entity_set_name = current_entity_name_from_fragment;
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Using entity name from metadata context fragment: " + entity_set_name);
    } else {
        // No fragment-based name available; do not guess from URL. Defer to metadata lookup below.
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "No entity name from @odata.context; deferring to metadata lookup");
    }
    
    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Final entity set name: " + entity_set_name);
    
    // If still empty, use metadata to find the single entity set name if unique
    if (entity_set_name.empty()) {
        auto edmx_probe = GetMetadata();
        auto sets = edmx_probe.FindEntitySets();
        if (sets.size() == 1) {
            entity_set_name = sets[0].name;
            ERPL_TRACE_DEBUG("ODATA_CLIENT", "Resolved single entity set from metadata: " + entity_set_name);
        } else {
            // Prefer the entity set name extracted earlier from @odata.context fragment if available
            if (!current_entity_name_from_fragment.empty()) {
                entity_set_name = current_entity_name_from_fragment;
                ERPL_TRACE_DEBUG("ODATA_CLIENT", "Using entity set from @odata.context fragment: " + entity_set_name);
            } else {
                // For non-Datasphere services, derive entity set name from URL path when multiple sets exist
                const auto url_str = url.ToString();
                const bool is_datasphere = (url_str.find("hcs.cloud.sap") != std::string::npos) ||
                                           (url_str.find("/api/v1/dwc/consumption/relational/") != std::string::npos);
                if (!is_datasphere) {
                    auto path = url.Path();
                    // Remove trailing slash
                    if (!path.empty() && path.back() == '/') {
                        path.pop_back();
                    }
                    // Extract last segment
                    auto last_slash = path.find_last_of('/');
                    std::string candidate = (last_slash == std::string::npos) ? path : path.substr(last_slash + 1);
                    ERPL_TRACE_DEBUG("ODATA_CLIENT", "Derived entity set candidate from URL: " + candidate);
                    // Validate against metadata entity sets
                    bool found = false;
                    for (const auto &es : sets) {
                        if (es.name == candidate) { found = true; break; }
                    }
                    if (found) {
                        entity_set_name = candidate;
                        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Resolved entity set from URL path: " + entity_set_name);
                    } else {
                        throw std::runtime_error("Unable to resolve entity set from @odata.context or URL; metadata has multiple sets");
                    }
                } else {
                    throw std::runtime_error("Unable to resolve entity set from @odata.context and metadata has multiple sets");
                }
            }
        }
    }
    auto edmx = GetMetadata();
    auto entity_set_type = edmx.FindEntitySet(entity_set_name);
    return entity_set_type;
}

EntityType ODataEntitySetClient::GetCurrentEntityType()
{
    auto entity_set_type = GetCurrentEntitySetType();
    auto edmx = GetMetadata();

    // Resolve the base entity type from the entity set
    std::string resolved_entity_type_name = entity_set_type.entity_type_name; // e.g., StandaloneService.flights_viewParameters

    // Datasphere parameterized pattern: when addressing ...(<params>)/Set, the result type is the
    // navigation property "Set" of the parameters entity, typically Collection(StandaloneService.<entity>Type)
    auto path_has_set = false;
    {
        auto p = url.Path();
        if (!p.empty()) {
            if (p.size() >= 4 && p.substr(p.size() - 4) == "/Set") {
                path_has_set = true;
            }
            if (p.find(")/Set") != std::string::npos) {
                path_has_set = true;
            }
        }
    }

    // Prefer explicit signal from @odata.context fragment if present
    if (current_response) {
        auto ctx = current_response->MetadataContextUrl();
        auto hash_pos = ctx.find('#');
        if (hash_pos != std::string::npos) {
            auto fragment = ctx.substr(hash_pos + 1);
            if (fragment.find("/Set") != std::string::npos) {
                path_has_set = true;
            }
        }
    }

    if (path_has_set || HasInputParameters()) {
        // Resolve parameters entity and then follow nav property "Set"
        ERPL_TRACE_DEBUG("ODATA_CLIENT", "Resolving entity type via navigation property 'Set' from: " + resolved_entity_type_name);
        auto params_entity_type = std::get<EntityType>(edmx.FindType(resolved_entity_type_name));
        std::string nav_type_name;
        for (const auto &nav_prop : params_entity_type.navigation_properties) {
            if (nav_prop.name == "Set") {
                nav_type_name = nav_prop.type; // e.g., Collection(StandaloneService.flights_viewType)
                break;
            }
        }
        if (!nav_type_name.empty()) {
            // Strip Collection(...) if present
            if (nav_type_name.find("Collection(") == 0 && nav_type_name.back() == ')') {
                nav_type_name = nav_type_name.substr(std::string("Collection(").size(), nav_type_name.size() - std::string("Collection(").size() - 1);
            }
            resolved_entity_type_name = nav_type_name;
            ERPL_TRACE_INFO("ODATA_CLIENT", "Resolved result entity type via 'Set': " + resolved_entity_type_name);
        } else {
            ERPL_TRACE_WARN("ODATA_CLIENT", "Navigation property 'Set' not found on type: " + params_entity_type.name + "; falling back to entity set type");
        }
    }

    auto entity_type = std::get<EntityType>(edmx.FindType(resolved_entity_type_name));
    return entity_type;
}

std::vector<std::string> ODataEntitySetClient::GetResultNames()
{
    auto edmx = GetMetadata();
    auto entity_set_type = GetCurrentEntitySetType();
    auto entity_type = GetCurrentEntityType();

    auto type_conv = DuckTypeConverter(edmx);
    auto entity_struct = type_conv(GetCurrentEntityType());

    std::vector<std::string> ret_names;
    for (const auto& child : StructType::GetChildTypes(entity_struct)) {
        ret_names.push_back(duckdb_compat::Str(child.first));
    }

    return ret_names;
}

std::vector<duckdb::LogicalType> ODataEntitySetClient::GetResultTypes()
{
    auto edmx = GetMetadata();
    auto type_conv = DuckTypeConverter(edmx);
    auto entity_struct = type_conv(GetCurrentEntityType());

    std::vector<duckdb::LogicalType> ret_types;
    for (const auto& child : StructType::GetChildTypes(entity_struct)) {
        ret_types.push_back(child.second);
    }

    return ret_types;
}

void ODataEntitySetClient::SetInputParameters(const std::map<std::string, std::string>& input_params)
{
    input_parameters = input_params;
    ERPL_TRACE_INFO("ODATA_CLIENT", "SetInputParameters called - storing " + std::to_string(input_params.size()) + " input parameters for OData client at " + std::to_string(reinterpret_cast<uintptr_t>(this)));
    for (const auto& [key, value] : input_params) {
        ERPL_TRACE_INFO("ODATA_CLIENT", "  Parameter: " + key + " = " + value);
    }
}

HttpUrl ODataEntitySetClient::AddInputParametersToUrl(const HttpUrl& url) const
{
    ERPL_TRACE_INFO("ODATA_CLIENT", "AddInputParametersToUrl called with " + std::to_string(input_parameters.size()) + " parameters on client at " + std::to_string(reinterpret_cast<uintptr_t>(this)));
    if (input_parameters.empty()) {
        ERPL_TRACE_INFO("ODATA_CLIENT", "No input parameters to add");
        return url;
    }
    InputParametersFormatter fmt;
    auto modified = fmt.addParams(url, input_parameters);
    if (modified.ToString() != url.ToString()) {
        ERPL_TRACE_INFO("ODATA_CLIENT", "Added input parameters to URL path: " + modified.ToString());
    }
    return modified;
}

// ----------------------------------------------------------------------

ODataServiceResponse::ODataServiceResponse(std::unique_ptr<HttpResponse> http_response, ODataVersion odata_version)
    : ODataResponse(std::move(http_response))
    , odata_version(odata_version)
{ 
    ERPL_TRACE_DEBUG("ODATA_RESPONSE", "Created OData service response");
    ERPL_TRACE_DEBUG("ODATA_RESPONSE", "Response content type: " + ContentType());
    ERPL_TRACE_DEBUG("ODATA_RESPONSE", std::string("OData version: ") + (odata_version == ODataVersion::V2 ? "V2" : "V4"));
}

std::shared_ptr<ODataServiceContent> ODataServiceResponse::CreateODataContent(const std::string& content, ODataVersion odata_version)
{
    if (ODataJsonContentMixin::IsJsonContentType(ContentType())) {
        ERPL_TRACE_DEBUG("ODATA_CONTENT", std::string("Creating JSON content with OData version: ") + (odata_version == ODataVersion::V2 ? "V2" : "V4"));
        auto content_obj = std::make_shared<ODataServiceJsonContent>(content);
        content_obj->SetODataVersion(odata_version);
        return content_obj;
    }
        
    throw std::runtime_error("Unsupported OData content type: " + ContentType());
}

std::string ODataServiceResponse::MetadataContextUrl()
{
    return Content()->MetadataContextUrl();
}

std::vector<ODataEntitySetReference> ODataServiceResponse::EntitySets()
{
    return Content()->EntitySets();
}

// ----------------------------------------------------------------------

ODataServiceClient::ODataServiceClient(std::shared_ptr<HttpClient> http_client, const HttpUrl& url)
    : ODataClient(http_client, url, nullptr)
{ }

ODataServiceClient::ODataServiceClient(std::shared_ptr<HttpClient> http_client, const HttpUrl& url, std::shared_ptr<HttpAuthParams> auth_params)
    : ODataClient(http_client, url, auth_params)
{ }

std::shared_ptr<ODataServiceResponse> ODataServiceClient::Get(bool get_next)
{
    if (current_response != nullptr) {
        return current_response;
    }

    auto http_response = DoHttpGet(url);
    current_response = std::make_shared<ODataServiceResponse>(std::move(http_response), odata_version);

    return current_response;
}

std::string ODataServiceClient::GetMetadataContextUrl()
{
    Get();
    return current_response->MetadataContextUrl();
}

Edmx ODataServiceClient::GetMetadata()
{
    ERPL_TRACE_INFO("ODATA_CLIENT", "ODataServiceClient::GetMetadata() called - handling V2/V4 compatibility");
    
    // Try to get the metadata context URL from service root first
    std::string metadata_url;
    try {
        metadata_url = GetMetadataContextUrl();
    } catch (const std::exception& e) {
        // If service root fails (e.g., V2 service with V4 headers), fall back to direct $metadata URL
        ERPL_TRACE_WARN("ODATA_CLIENT", "Service root request failed: " + std::string(e.what()) + ", falling back to direct $metadata URL");
        metadata_url = url.ToString();
        if (!metadata_url.empty() && metadata_url.back() != '/') {
            metadata_url += "/";
        }
        metadata_url += "$metadata";
    }
    
    // Check cache first
    auto cached_edmx = EdmCache::GetInstance().Get(metadata_url);
    if (cached_edmx) {
        return *cached_edmx;
    }

    // Fetch metadata directly
    auto metadata_response = DoMetadataHttpGet(metadata_url);
    auto content = metadata_response->Content();
    auto edmx = Edmx::FromXml(content);
    
    // Auto-detect version from metadata if not already set
    if (odata_version == ODataVersion::UNKNOWN) {
        odata_version = edmx.GetVersion();
        ERPL_TRACE_INFO("ODATA_CLIENT", "Detected OData version from metadata: " + std::string(odata_version == ODataVersion::V2 ? "V2" : "V4"));
    }

    EdmCache::GetInstance().Set(metadata_url, edmx);
    return edmx;
}

// -------------------------------------------------------------------------------------------------
// ODataClientFactory Implementation
// -------------------------------------------------------------------------------------------------

ODataClientFactory::ProbeResult ODataClientFactory::ProbeUrl(const std::string& url, std::shared_ptr<HttpAuthParams> auth_params, std::optional<uint32_t> max_page_size)
{
    ERPL_TRACE_DEBUG("ODATA_FACTORY", "Probing URL: " + url);
    
    // Create HTTP client with URL encoding disabled for OData (V2 & V4)
    HttpParams http_params;
    http_params.url_encode = false;
    auto http_client = std::make_shared<HttpClient>(http_params);
    
    // Normalize URL: preserve existing query parameters and ensure $format=json is present
    HttpUrl normalized_url(url);

    // Rebuild query to preserve keys and encode $filter values
    try {
        auto qmark = url.find('?');
        if (qmark != std::string::npos && qmark + 1 < url.size()) {
            std::string original_query = url.substr(qmark + 1);
            std::istringstream iss(original_query);
            std::string param;
            std::vector<std::pair<std::string, std::string>> kv_pairs;

            while (std::getline(iss, param, '&')) {
                if (param.empty()) {
                    continue;
                }
                size_t eq = param.find('=');
                std::string raw_key = (eq == std::string::npos) ? param : param.substr(0, eq);
                std::string raw_value = (eq == std::string::npos) ? std::string("") : param.substr(eq + 1);

                // Decode key and canonicalize common aliases
                std::string decoded_key = ODataUrlCodec::decodeQueryValue(raw_key);
                if (!decoded_key.empty() && decoded_key.rfind("%24", 0) == 0) {
                    decoded_key = std::string("$") + decoded_key.substr(3);
                }
                if (decoded_key == "filter") {
                    decoded_key = "$filter";
                }
                if (decoded_key == "expand") {
                    decoded_key = "$expand";
                }
                if (decoded_key == "select") {
                    decoded_key = "$select";
                }
                if (decoded_key == "top") {
                    decoded_key = "$top";
                }
                if (decoded_key == "skip") {
                    decoded_key = "$skip";
                }
                if (decoded_key == "format") {
                    decoded_key = "$format";
                }

                // Decode the value, then re-encode it for the option it belongs to. Passing
                // raw_value through was the same defect as in the predicate pushdown: the
                // value has been decoded for inspection, and emitting the original bytes
                // for everything except $filter meant a caller's %20 survived here but was
                // mangled by whichever site rebuilt the query next. One encoder, both
                // sites. See GitHub #227.
                std::string decoded_value = ODataUrlCodec::decodeQueryValue(raw_value);
                std::string final_value =
                    ODataUrlCodec::encodeQueryValueForOption(decoded_key, decoded_value);

                // If we failed to decode a meaningful key, keep the raw key to avoid losing information
                std::string final_key = decoded_key.empty() ? raw_key : decoded_key;
                kv_pairs.emplace_back(final_key, final_value);
            }

            if (!kv_pairs.empty()) {
                std::ostringstream rebuilt;
                rebuilt << "?";
                for (size_t i = 0; i < kv_pairs.size(); ++i) {
                    if (i > 0) {
                        rebuilt << "&";
                    }
                    rebuilt << kv_pairs[i].first << "=" << kv_pairs[i].second;
                }
                normalized_url.Query(rebuilt.str());
            }
        }
    } catch (...) {
        // If anything goes wrong during query normalization, fall back to original
    }

    // Ensure JSON format is present (idempotent)
    ODataUrlCodec::ensureJsonFormat(normalized_url);
    
    // This request is built here and never goes through ODataClient::DoHttpGet, so it
    // needs its own check - the claim that DoHttpGet is "the single point every OData
    // request passes through" was false for exactly this function. The URL is the
    // CALLER's, so control characters only, matching that choke point.
    // Scheme, host and port included, not just the path and query. #229 added this check
    // with ToPathQuery() alone and #230 widened its two siblings without widening this one -
    // the same narrowing, fixed at two of three sites. The host goes out in the Host header
    // and, behind a proxy, in the request line.
    //
    // The URL is the CALLER's here, so control characters only. A raw space is no longer
    // something this code produces (#227 made both query rebuilds encode on the way out),
    // but the check stays deliberately narrow: a caller's own URL is not the place to be
    // strict about a character that cannot forge a header. Service-supplied links are held
    // to IsWireSafeUrl, which refuses spaces too, at their own seams.
    const auto probe_target = WireTargetOf(normalized_url);
    if (!HasNoControlCharacters(probe_target)) {
        throw duckdb::IOException(
            "Refusing to probe a URL containing control characters: '%s'.",
            SummariseUrlForMessage(probe_target));
    }

    // Make a single HTTP request to probe the content
    auto http_request = HttpRequest(HttpMethod::GET, normalized_url);
    
    // Don't add OData version headers for the probe - let the service respond naturally
    if (auth_params != nullptr) {
        http_request.AuthHeadersFromParams(*auth_params);
    }

    // The probe body becomes page one, so the caller's page-size preference has to travel
    // with it or the first page is fetched at the service's default size (GitHub #185).
    if (max_page_size.has_value()) {
        http_request.headers["Prefer"] =
            "odata.maxpagesize=" + std::to_string(max_page_size.value());
    }
    
    auto http_response = http_client->SendRequest(http_request);
    
    if (http_response == nullptr || http_response->Code() != 200) {
        std::stringstream ss;
        int status_code = http_response ? http_response->Code() : 0;
        
        // Provide more specific error messages based on HTTP status code
        switch (status_code) {
            case 0:
                ss << "Failed to connect to OData service at: " << url 
                   << " (Connection failed - check if the server is running and accessible)";
                break;
            case 401:
                ss << "Authentication failed for OData service at: " << url 
                   << " (HTTP 401 - check your credentials in the secret)";
                break;
            case 403:
                ss << "Access forbidden to OData service at: " << url 
                   << " (HTTP 403 - check if your user has permission to access this service)";
                break;
            case 404:
                ss << "OData service not found at: " << url 
                   << " (HTTP 404 - check if the URL path is correct, especially the entity set name)";
                break;
            case 500:
                ss << "Internal server error from OData service at: " << url 
                   << " (HTTP 500 - the SAP system encountered an error)";
                break;
            default:
                ss << "Failed to access OData service at: " << url 
                   << " (HTTP " << status_code << " - unexpected server response)";
                break;
        }
        
        throw std::runtime_error(ss.str());
    }
    
    std::string content = http_response->Content();
    std::string content_type = http_response->ContentType();
    
    // Detect OData version from response
    ODataVersion version = DetectVersionFromResponse(content, content_type);
    
    // Detect if this is a service root document
    bool is_service_root = IsServiceRootResponse(content);
    
    ERPL_TRACE_INFO("ODATA_FACTORY", "Probe result - Version: " + std::to_string(static_cast<int>(version)) + 
                     ", IsServiceRoot: " + (is_service_root ? "true" : "false"));
    ERPL_TRACE_INFO("ODATA_FACTORY", "Content preview: " + content.substr(0, 200) + "...");
    
    ProbeResult result;
    result.version = version;
    result.is_service_root = is_service_root;
    result.initial_content = content;
    result.normalized_url = normalized_url;
    result.auth_params = auth_params;
    return result;
}

std::shared_ptr<ODataEntitySetClient> ODataClientFactory::CreateEntitySetClient(const ProbeResult& result)
{
    ERPL_TRACE_DEBUG("ODATA_FACTORY", "Creating ODataEntitySetClient");
    
    // Create HTTP client with URL encoding disabled for OData (V2 & V4)
    HttpParams http_params;
    http_params.url_encode = false;
    auto http_client = std::make_shared<HttpClient>(http_params);
    
    auto client = std::make_shared<ODataEntitySetClient>(http_client, result.normalized_url, result.auth_params);
    
    // Set the detected OData version
    client->SetODataVersion(result.version);
    
    return client;
}

std::shared_ptr<ODataServiceClient> ODataClientFactory::CreateServiceClient(const ProbeResult& result)
{
    ERPL_TRACE_DEBUG("ODATA_FACTORY", "Creating ODataServiceClient");
    
    // Create HTTP client with URL encoding disabled for OData (V2 & V4)
    HttpParams http_params;
    http_params.url_encode = false;
    auto http_client = std::make_shared<HttpClient>(http_params);
    
    auto client = std::make_shared<ODataServiceClient>(http_client, result.normalized_url, result.auth_params);
    
    // Set the detected OData version
    client->SetODataVersion(result.version);
    
    return client;
}

ODataVersion ODataClientFactory::DetectVersionFromResponse(const std::string& content, const std::string& content_type)
{
    // Prefer structured JSON-based detection to handle service documents
    try {
        return ODataJsonContentMixin::DetectODataVersion(content);
    } catch (...) {
        // Fallback: simple heuristics
        if (content.find("@odata.context") != std::string::npos) {
            return ODataVersion::V4;
        }
        if (content.find("__metadata") != std::string::npos) {
            return ODataVersion::V2;
        }
        if (content_type.find("application/json") != std::string::npos) {
            return ODataVersion::V4;
        }
        return ODataVersion::V4;
    }
}

bool ODataClientFactory::IsServiceRootResponse(const std::string& content)
{
    // Check for service root document patterns
    // Service root documents contain entity set references, not actual entity data
    // V4: has "value" array with objects that have "name" and "url" properties
    // V2: has "d" object with "results" array containing entity set references
    
    try {
        auto doc = yyjson_read(content.c_str(), content.length(), 0);
        if (!doc) {
            return false;
        }
        
        auto root = yyjson_doc_get_root(doc);
        if (!root) {
            yyjson_doc_free(doc);
            return false;
        }
        
        // V4 service root: has "value" array with entity set references
        auto value_arr = yyjson_obj_get(root, "value");
        if (value_arr && yyjson_is_arr(value_arr)) {
            // Check if this is a service root by looking at the first item
            auto first_item = yyjson_arr_get_first(value_arr);
            if (first_item && yyjson_is_obj(first_item)) {
                // Service root items have "name" and "url" properties
                // Entity set items have actual entity properties
                bool has_name = yyjson_obj_get(first_item, "name") != nullptr;
                bool has_url = yyjson_obj_get(first_item, "url") != nullptr;
                
                // If it has name/url, it's likely a service root
                if (has_name && has_url) {
                    yyjson_doc_free(doc);
                    return true;
                }
            }
        }
        
        // V2 service root: has "d" with "EntitySets" array of strings
        auto d_obj = yyjson_obj_get(root, "d");
        if (d_obj && yyjson_is_obj(d_obj)) {
            auto entity_sets_arr = yyjson_obj_get(d_obj, "EntitySets");
            if (entity_sets_arr && yyjson_is_arr(entity_sets_arr)) {
                // V2 service root has EntitySets array with string values
                auto first_item = yyjson_arr_get_first(entity_sets_arr);
                if (first_item && yyjson_is_str(first_item)) {
                    // If EntitySets contains string values, it's a V2 service root
                    yyjson_doc_free(doc);
                    return true;
                }
            }
            
            // Also check for V2 service root with "results" containing entity set references
            auto results_arr = yyjson_obj_get(d_obj, "results");
            if (results_arr && yyjson_is_arr(results_arr)) {
                // Check if this is a service root by looking at the first item
                auto first_item = yyjson_arr_get_first(results_arr);
                if (first_item && yyjson_is_obj(first_item)) {
                    // Service root items have "Name" and "Url" properties (V2 uses capital letters)
                    bool has_name = yyjson_obj_get(first_item, "Name") != nullptr;
                    bool has_url = yyjson_obj_get(first_item, "Url") != nullptr;
                    
                    // If it has Name/Url, it's likely a service root
                    if (has_name && has_url) {
                        yyjson_doc_free(doc);
                        return true;
                    }
                }
            }
        }
        
        // Direct "results" array (V2 pattern)
        auto results_arr = yyjson_obj_get(root, "results");
        if (results_arr && yyjson_is_arr(results_arr)) {
            // Check if this is a service root by looking at the first item
            auto first_item = yyjson_arr_get_first(results_arr);
            if (first_item && yyjson_is_obj(first_item)) {
                // Service root items have "Name" and "Url" properties (V2 uses capital letters)
                bool has_name = yyjson_obj_get(first_item, "Name") != nullptr;
                bool has_url = yyjson_obj_get(first_item, "Url") != nullptr;
                
                // If it has Name/Url, it's likely a service root
                if (has_name && has_url) {
                    yyjson_doc_free(doc);
                    return true;
                }
            }
        }
        
        yyjson_doc_free(doc);
        return false;
        
    } catch (...) {
        return false;
    }
}

} // namespace erpl_web
