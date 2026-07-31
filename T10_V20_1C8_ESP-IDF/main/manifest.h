// File manifest for device→phone sync: enumerates syncable files on the SD card
// and serves the JSON manifest defined in docs/DEVICE_FILE_SYNC.md §4. Shared by
// the HTTP file server and (later) the BLE control channel. SD must be mounted.
#pragma once

#include <stdbool.h>

// Build the manifest JSON into `out` (capacity `cap`). Returns the length written,
// or -1 on error. Lazily computes and caches a sha256 sidecar (<name>.s256) per
// file, so repeat calls are cheap.
int manifest_build_json(char *out, int cap);

// Map a manifest id (stable within a directory scan) to its /sdcard path.
bool manifest_path_for_id(int id, char *path, int cap);

// 64-char lowercase-hex sha256 for a file id (from the cached sidecar). Returns
// false if the id is unknown.
bool manifest_sha256_for_id(int id, char *hex64, int cap);

// Delete a file (and its sha256 sidecar) by id. Returns false if not found.
bool manifest_delete_id(int id);

// Compute + cache any missing sha256 sidecars (SLOW — streams every file off SD).
// Run from a background task so the manifest never blocks on hashing.
void manifest_precache(void);
