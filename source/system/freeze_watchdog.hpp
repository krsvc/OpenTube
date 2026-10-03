#pragma once
// DIAGNOSTIC ONLY (NETWORK_FREEZE_DELIVERY.md). Low-cost watchdog thread on core 1: when the main loop heartbeat
// (util/freeze_diag.hpp) stops for 5 s it appends one line of thread phases / lock owners to
// DEF_MAIN_DIR "freeze_diag.log" (at most 8 lines per run, file capped at 32 KiB, literals + integers only).
// It reads breadcrumbs only; it never takes an app lock, allocates, or changes behaviour.
void freeze_watchdog_start();
void freeze_watchdog_stop();
