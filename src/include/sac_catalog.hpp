#pragma once

#include "duckdb.hpp"
#include "datazoo/oauth2/http_client.hpp"
#include "odata_client.hpp"
#include <memory>
#include <vector>
#include <string>

namespace erpl_web {

// Forward declarations
struct SacAuthParams;

/**
 * SAC Model Information
 * Represents a planning model or analytics model in SAC
 */
struct SacModel {
    std::string id;                    // Technical model ID
    std::string name;                  // Business name
    std::string description;           // Model description
    std::string type;                  // 'PLANNING' or 'ANALYTICS'
    std::string owner;                 // Model owner
    std::string created_at;            // Creation timestamp
    std::string last_modified_at;      // Last modification timestamp
    std::vector<std::string> dimensions; // Available dimensions
    std::vector<std::string> measures;  // Available measures (for ANALYTICS)
};

/**
 * SAC Story Information
 * Represents a story or dashboard in SAC
 */
struct SacStory {
    std::string id;                    // Technical story ID
    std::string name;                  // Story name
    std::string description;           // Story description
    std::string owner;                 // Story owner
    std::string created_at;            // Creation timestamp
    std::string last_modified_at;      // Last modification timestamp
    std::string status;                // 'DRAFT' or 'PUBLISHED'
};

/**
 * SAC Catalog Service
 * Provides discovery and metadata retrieval for SAC objects
 * Uses OData v4 APIs to query models, stories, and dimensions
 */
class SacCatalogService {
public:
    SacCatalogService(
        const std::string& tenant,
        const std::string& region,
        std::shared_ptr<HttpAuthParams> auth_params);

    ~SacCatalogService();

    // Model discovery

    /**
     * List all accessible models
     */
    std::vector<SacModel> ListModels() const;

    /**
     * Get model by ID
     */
    std::optional<SacModel> GetModel(const std::string& model_id) const;

    // Story discovery

    /**
     * List all accessible stories
     */
    std::vector<SacStory> ListStories() const;

    /**
     * Get story by ID
     */
    std::optional<SacStory> GetStory(const std::string& story_id) const;

private:
    std::string tenant_;
    std::string region_;
    std::shared_ptr<HttpAuthParams> auth_params_;
    std::shared_ptr<ODataServiceClient> catalog_client_;
};

/**
 * DuckDB Table Functions for SAC Catalog
 */

// sac_show_models() -> Shows all accessible models
duckdb::TableFunctionSet CreateSacShowModelsFunction();

// sac_show_stories() -> Shows all accessible stories
duckdb::TableFunctionSet CreateSacShowStoriesFunction();

// sac_describe_model(model_id) -> Get model metadata and dimensions
duckdb::TableFunctionSet CreateSacGetModelInfoFunction();

// sac_describe_story(story_id) -> Get story metadata
duckdb::TableFunctionSet CreateSacGetStoryInfoFunction();

} // namespace erpl_web
