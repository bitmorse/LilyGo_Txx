#include "manifest_filter.h"

#include <string.h>
#include <strings.h>   // strcasecmp

// Only .mcap recordings appear in the sync manifest. Hidden/dotfiles are dropped
// first (that also kills "." / ".." / ".mcap"); everything else must end in a
// case-insensitive ".mcap" -- which naturally excludes .s256 sidecars, .wav, etc.
bool manifest_include_name(const char *name)
{
    if (name == NULL || name[0] == '.') return false;   // hidden / dotfiles
    size_t len = strlen(name);
    return len >= 5 && strcasecmp(name + len - 5, ".mcap") == 0;
}
