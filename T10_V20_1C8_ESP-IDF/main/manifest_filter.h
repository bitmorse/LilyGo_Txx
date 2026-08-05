#ifndef MANIFEST_FILTER_H
#define MANIFEST_FILTER_H

#include <stdbool.h>

// Decide whether an SD filename belongs in the File Sync manifest.
// Policy: only .mcap recordings (case-insensitive extension); never hidden /
// dotfiles (".", "..", ".foo"), and never sidecars (.s256) or any other type.
// Pure + host-tested so the extension rule has real coverage (see test/).
bool manifest_include_name(const char *name);

#endif // MANIFEST_FILTER_H
