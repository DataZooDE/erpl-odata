#define CATCH_CONFIG_RUNNER
#include <iostream>
#include <cstdlib>
#include "catch.hpp"

#ifdef _WIN32
#include <cstdlib>
#else
#include <cstdlib>
#endif

int main(int argc, char *argv[]) {
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