#pragma once

#include <cstdlib>
#include <string>

inline std::string ErplOdataLoadSql() {
#ifdef ODATA_TEST_STATIC_ERPL
    // The binary links erpl_odata on all supported lines. Loading the path as
    // well would create a second copy of its globals in the same process.
    return "LOAD erpl_odata";
#else
    const char *path = std::getenv("ODATA_TEST_EXTENSION_PATH");
    if (!path || !*path) {
        return "LOAD erpl_odata";
    }
    std::string escaped;
    for (const char *current = path; *current; ++current) {
        escaped += *current;
        if (*current == '\'') {
            escaped += '\'';
        }
    }
    return "LOAD '" + escaped + "'";
#endif
}
