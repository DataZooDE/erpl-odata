#pragma once

#include "datazoo_banner_duckdb.hpp"

// Shared identity for the load banner and the issue-link error footer.
// External linkage: DATAZOO_GUARD takes the address as a non-type template
// argument. Defined in erpl_odata_extension.cpp.
extern const datazoo::BannerInfo ERPL_ODATA_BANNER;
