// File manifest for device→phone sync: enumerates syncable files on the SD card
// and serves the JSON manifest defined in docs/DEVICE_FILE_SYNC.md §4. Shared by
// the HTTP file server and (later) the BLE control channel. SD must be mounted.
#pragma once

#include <stdbool.h>

// Create the internal scan lock. Call once, single-threaded, before the HTTP
// handlers or the precache task can call any other function here.
void manifest_init(void);

// Pause/resume the background sha256 precache. The HTTP file handler holds this
// during a transfer so a client download gets the full SD bus (no hashing races).
void manifest_precache_hold(bool hold);

// Stream the manifest JSON through `sink` (called with successive chunks; return 0
// to continue, non-zero to abort). Streaming instead of one big buffer keeps the
// whole file list off DRAM (no per-request size ceiling, no 20 KB static buffer).
// sha256 fields come from cached sidecars (never computed here); a file not yet
// hashed omits its sha256. Returns 0 on success, -1 if `sink` aborted (partial
// output already emitted -- the socket is closing anyway).
typedef int (*manifest_sink_fn)(void *ctx, const char *data, int len);
int manifest_stream_json(manifest_sink_fn sink, void *ctx);

// Map a manifest id (stable within a directory scan) to its /sdcard path.
bool manifest_path_for_id(int id, char *path, int cap);

// 64-char lowercase-hex sha256 for a file id (from the cached sidecar only, never
// computed). Returns false if the id is unknown or not yet hashed by the precache.
bool manifest_sha256_for_id(int id, char *hex64, int cap);

// Delete a file (and its sha256 sidecar) by id. Returns false if not found.
bool manifest_delete_id(int id);

// Compute + cache any missing sha256 sidecars (SLOW — streams every file off SD).
// Run from a background task so the manifest never blocks on hashing.
void manifest_precache(void);
