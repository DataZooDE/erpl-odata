#define CATCH_CONFIG_RUNNER
#include <iostream>
#include <cstdlib>
#include "catch.hpp"
#ifdef ODATA_TEST_HAS_LOADABLE_LIBRARY
#include "duckdb_static_extension.h"
#include "erpl_odata_extension.hpp"
#include "core_functions_extension.hpp"
#include "json_extension.hpp"
#include "parquet_extension.hpp"
extern "C" int32_t duckdb_extension_loadable_extensions_describe(duckdb_extension_descriptor *);
#endif


int main(int argc, char *argv[]) {
#ifdef ODATA_TEST_HAS_LOADABLE_LIBRARY
  if (duckdb_register_static_extension(duckdb_extension_loadable_extensions_describe) != 0) {
    return 1;
  }
  if (duckdb_register_static_extension(
          &duckdb::StaticExtensionDescriber<duckdb::ErplOdataExtension>::Describe) != 0) {
    return 1;
  }
  if (duckdb_register_static_extension(
          &duckdb::StaticExtensionDescriber<duckdb::CoreFunctionsExtension>::Describe) != 0 ||
      duckdb_register_static_extension(
          &duckdb::StaticExtensionDescriber<duckdb::JsonExtension>::Describe) != 0 ||
      duckdb_register_static_extension(
          &duckdb::StaticExtensionDescriber<duckdb::ParquetExtension>::Describe) != 0) {
    return 1;
  }
#endif
  // Disable/suppress sanitizer checks for the test run
  // - disable ODR detection (causes false positives when mixing libs)
  // - disable leak detection (OpenSSL/httplib noise)
  // - avoid aborting on sanitizer reports so tests can proceed
#ifdef _WIN32
  _putenv_s("ASAN_OPTIONS", "detect_odr_violation=0:detect_leaks=0:halt_on_error=0:abort_on_error=0");
  _putenv_s("UBSAN_OPTIONS", "halt_on_error=0:print_stacktrace=0");
#else
  setenv("ASAN_OPTIONS", "detect_odr_violation=0:detect_leaks=0:halt_on_error=0:abort_on_error=0", 0);
  setenv("UBSAN_OPTIONS", "halt_on_error=0:print_stacktrace=0", 0);
#endif

  std::cout << std::endl << "**** ERPL ODATA CPP Unit Tests ****" << std::endl << std::endl;

  int result = Catch::Session().run( argc, argv );
  return result;
}
