// The REAL source/definitions.hpp, compiled once as the OpenTube release build (-DDEF_DIAG_BUILD, no version flags:
// the fallback micro must match the Makefile) and once as the plain upstream build. Missing macros are reported as
// failures, not compile errors, so the same file shows RED against the preserved baseline.
#include <cstdio>
#include <string>
#include "definitions.hpp"

int main() {
	int passed = 0, failed = 0;
	auto check = [&](bool ok, const std::string &what) {
		ok ? passed++ : failed++;
		if (!ok) {
			printf("    FAIL %s\n", what.c_str());
		}
	};
#ifdef DEF_DIAG_BUILD
	printf("OpenTube (diag) build: DEF_MAIN_DIR=%s\n", (DEF_MAIN_DIR).c_str());
	check(DEF_MAIN_DIR == "/3ds/opentube/", "OpenTube saves under /3ds/opentube/ (got " + DEF_MAIN_DIR + ")");
	check(DEF_PLAYBACK_DIAG_LOG_PATH == "/3ds/opentube/playback_diag.log", "diagnostic log under the new root");
#ifdef DEF_PREDECESSOR_MAIN_DIR
	check(DEF_PREDECESSOR_MAIN_DIR == "/3ds/FourthTubeTest/", "migration reads only /3ds/FourthTubeTest/");
#else
	check(false, "DEF_PREDECESSOR_MAIN_DIR defined");
#endif
#ifdef DEF_DATA_ROOT_STAGE_DIR
	check(DEF_DATA_ROOT_STAGE_DIR == "/3ds/opentube.partial/", "partial output in /3ds/opentube.partial/");
#else
	check(false, "DEF_DATA_ROOT_STAGE_DIR defined");
#endif
	check(OPENTUBE_VERSION_MINOR == 16 && OPENTUBE_VERSION_MICRO == 2, "fallback version 0.16.2");
	check(((OPENTUBE_VERSION_MINOR << 10) | OPENTUBE_VERSION_MICRO) == 16386, "CIA title version 16386");
#else
	printf("upstream build: DEF_MAIN_DIR=%s\n", (DEF_MAIN_DIR).c_str());
	check(DEF_MAIN_DIR == "/3ds/FourthTube/", "plain upstream make keeps /3ds/FourthTube/");
#ifdef DEF_PREDECESSOR_MAIN_DIR
	check(false, "no migration source in the upstream build");
#endif
#endif
	printf("data_root defs: %d checks passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
