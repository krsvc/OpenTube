#include "headers.hpp"
#include "util/freeze_diag.hpp"

int main() {
	Menu_init();

	// Main loop
	while (aptMainLoop()) {
		if (!Menu_main()) {
			break;
		}
		freeze_diag::phase("main:apt"); // next: aptMainLoop() (HOME / sleep handling, APT hooks)
	}

	// menu exit or system close: cancel, then wait (responsive, never a blocking join) until the download worker and
	// the async thread (stream resolution) really ended; only then tear down and return to the runtime
	Menu_exit_request();
	while (!Menu_exit_ready()) {
		Menu_stopping();
	}
	Menu_exit();
	return 0;
}
