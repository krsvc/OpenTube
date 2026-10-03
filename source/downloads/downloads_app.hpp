#pragma once
// App wiring of the SD download library (3DS only): the single DownloadManager, its worker thread and the
// stream resolution through the existing async task thread (the YouTube parser's network session is not
// thread-safe, so the worker never calls the parser itself).
#include "downloads/download_manager.hpp"
#include "downloads/download_options.hpp"

void Downloads_start(); // Menu_init: create the manager and start its worker (scans the library off the main thread)
// exit request (does not wait): no new jobs, the in-flight chunk and any pause / resolve wait are aborted and a queued
// stream resolution is dropped
void Downloads_request_stop();
// true once the kernel has ended the worker thread (or it never existed); never blocks
bool Downloads_finished();
// Menu_exit: stop the worker (aborts an in-flight transfer) and join it with the usual bound. false = the worker is
// still live (e.g. inside a blocking DNS lookup): its thread handle, session and manager are kept, and the caller must
// not tear down anything it can still use (network sessions, sockets, SD card).
bool Downloads_stop();
downloads::DownloadManager &downloads_manager();
bool downloads_running();
// r14 saved thumbnails, shown through the thumbnail loader without any network request: the loader key of an item
// ("local-thumb:S:<id>" list tile, "local-thumb:L:<id>" top-screen hero) and the bounded, validated file read behind it
std::string Downloads_thumbnail_key(const std::string &item_id, bool large);
bool Downloads_parse_thumbnail_key(const std::string &key, std::string *item_id, bool *large);
bool Downloads_read_thumbnail(const std::string &item_id, std::vector<u8> *out); // any thread; false = none / invalid
