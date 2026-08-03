#include "manifest.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "mbedtls/sha256.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "manifest";

#define MOUNT     "/sdcard"
#define MAX_FILES 48      // list buffer is MAX_FILES*sizeof(entry_t); keep the
                          // malloc small so it succeeds in low-heap AP+BLE mode
#define NAME_MAX_ 64

typedef struct {
    char   name[NAME_MAX_];
    long   size;
    time_t mtime;
} entry_t;

// --- helpers ----------------------------------------------------------------

static bool is_sidecar_or_hidden(const char *n)
{
    if (n[0] == '.') return true;                       // hidden / dotfiles
    size_t len = strlen(n);
    return len >= 5 && strcmp(n + len - 5, ".s256") == 0;
}

static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".mcap")) return "application/mcap";
    if (!strcasecmp(dot, ".wav"))  return "audio/wav";
    if (!strcasecmp(dot, ".opus")) return "audio/opus";
    if (!strcasecmp(dot, ".mp3"))  return "audio/mpeg";
    if (!strcasecmp(dot, ".json")) return "application/json";
    return "application/octet-stream";
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

// Scan /sdcard into a sorted (by name) list of regular files. Returns the count.
static int scan(entry_t *list, int max)
{
    DIR *d = opendir(MOUNT);
    if (!d) { ESP_LOGW(TAG, "opendir(%s) failed", MOUNT); return 0; }

    int n = 0;
    struct dirent *de;
    while (n < max && (de = readdir(d)) != NULL) {
        if (de->d_type == DT_DIR) continue;
        if (is_sidecar_or_hidden(de->d_name)) continue;
        size_t nl = strlen(de->d_name);
        if (nl == 0 || nl >= NAME_MAX_) continue;       // bounds the copies below

        char path[300];                                 // fits MOUNT + any name
        snprintf(path, sizeof(path), "%s/%s", MOUNT, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        memcpy(list[n].name, de->d_name, nl + 1);       // nl < NAME_MAX_, fits
        list[n].size  = (long)st.st_size;
        list[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(list, n, sizeof(entry_t), cmp_name);          // stable id ordering
    return n;
}

// Heap-allocate the list (it is ~10 KB — too big for the HTTP task's stack) and
// scan into it. Caller frees *out. Returns the file count (0 and *out=NULL on OOM).
static int scan_alloc(entry_t **out)
{
    entry_t *l = malloc(sizeof(entry_t) * MAX_FILES);
    if (!l) { *out = NULL; return 0; }
    int n = scan(l, MAX_FILES);
    *out = l;
    return n;
}

// Compute sha256 of a file, streaming from SD. Writes 64 hex chars + NUL.
static bool compute_sha256(const char *path, char *hex)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    long remaining = (long)st.st_size;                  // bound the read: a corrupt
                                                        // FAT cluster loop must not
                                                        // spin forever here.
    // POSIX open/read, NOT stdio: newlib's tiny default file buffer makes fread
    // ~15x slower off SD (each read becomes many tiny SD transactions).
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);                     // 0 = SHA-256 (HW accel)

    uint8_t buf[2048];                                  // stack (compute can run on 2 tasks)
    ssize_t r;
    while (remaining > 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
        mbedtls_sha256_update(&ctx, buf, r);
        remaining -= (long)r;
    }
    close(fd);

    uint8_t out[32];
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
    hex[64] = '\0';
    return true;
}

// Read a fresh <name>.s256 sidecar WITHOUT computing (fast; safe on the HTTP task).
// Returns false if the sidecar is missing or stale.
static bool sha256_read_sidecar(const char *name, char *hex)
{
    char path[288], side[300];               // sized for a max-length name
    snprintf(path, sizeof(path), "%s/%s", MOUNT, name);
    snprintf(side, sizeof(side), "%s/%s.s256", MOUNT, name);

    struct stat fs, ss;
    if (stat(path, &fs) != 0) return false;
    if (stat(side, &ss) != 0 || ss.st_mtime < fs.st_mtime) return false;  // stale/missing
    FILE *s = fopen(side, "rb");
    if (!s) return false;
    size_t got = fread(hex, 1, 64, s);
    fclose(s);
    if (got != 64) return false;
    hex[64] = '\0';
    return true;
}

// Get sha256 hex, computing + caching the sidecar if needed. SLOW (streams the
// whole file off SD) — use only off the HTTP task (background precache, or a
// single /file ETag), never in the manifest loop.
static bool sha256_cached(const char *name, char *hex)
{
    if (sha256_read_sidecar(name, hex)) return true;
    char path[288], side[300];
    snprintf(path, sizeof(path), "%s/%s", MOUNT, name);
    snprintf(side, sizeof(side), "%s/%s.s256", MOUNT, name);
    if (!compute_sha256(path, hex)) return false;
    FILE *s = fopen(side, "wb");
    if (s) { fwrite(hex, 1, 64, s); fclose(s); }
    return true;
}

// --- public -----------------------------------------------------------------

int manifest_build_json(char *out, int cap)
{
    entry_t *list;
    int n = scan_alloc(&list);
    if (!list) return -1;

    unsigned long long freeb = 0;
    uint64_t total = 0, avail = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &avail) == ESP_OK)
        freeb = avail;

    int off = snprintf(out, cap, "{\"v\":1,\"free_bytes\":%llu,\"files\":[", freeb);
    for (int i = 0; i < n && off < cap - 256; i++) {
        char hex[65] = {0};
        bool have_hash = sha256_read_sidecar(list[i].name, hex);   // fast; no compute

        char iso[24];
        struct tm tmv;
        gmtime_r(&list[i].mtime, &tmv);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);

        off += snprintf(out + off, cap - off,
            "%s{\"id\":%d,\"name\":\"%s\",\"mime\":\"%s\",\"bytes\":%ld,"
            "\"created_at\":\"%s\"%s%s%s}",
            i ? "," : "", i, list[i].name, mime_for(list[i].name),
            list[i].size, iso,
            have_hash ? ",\"sha256\":\"" : "", have_hash ? hex : "",
            have_hash ? "\"" : "");
    }
    off += snprintf(out + off, cap - off, "]}");
    free(list);
    return (off < cap) ? off : -1;
}

bool manifest_path_for_id(int id, char *path, int cap)
{
    entry_t *list;
    int n = scan_alloc(&list);
    if (!list) return false;
    bool ok = (id >= 0 && id < n);
    if (ok) snprintf(path, cap, "%s/%s", MOUNT, list[id].name);
    free(list);
    return ok;
}

bool manifest_sha256_for_id(int id, char *hex64, int cap)
{
    entry_t *list;
    int n = scan_alloc(&list);
    if (!list) return false;
    bool ok = (id >= 0 && id < n && cap >= 65);
    if (ok) ok = sha256_cached(list[id].name, hex64);
    free(list);
    return ok;
}

void manifest_precache(void)
{
    entry_t *list;
    int n = scan_alloc(&list);
    if (!list) return;
    char hex[65];
    for (int i = 0; i < n; i++) {
        if (sha256_read_sidecar(list[i].name, hex)) continue;   // already cached
        int64_t t0 = esp_timer_get_time();
        if (sha256_cached(list[i].name, hex))                   // computes + caches
            ESP_LOGI(TAG, "sha256 %s in %lld ms", list[i].name,
                     (esp_timer_get_time() - t0) / 1000);
    }
    free(list);
    ESP_LOGI(TAG, "precache done (%d files)", n);
}

bool manifest_delete_id(int id)
{
    entry_t *list;
    int n = scan_alloc(&list);
    if (!list) return false;
    bool ok = (id >= 0 && id < n);
    if (ok) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", MOUNT, list[id].name);
        remove(path);
        snprintf(path, sizeof(path), "%s/%s.s256", MOUNT, list[id].name);
        remove(path);                                    // best-effort sidecar
        ESP_LOGI(TAG, "deleted id %d (%s)", id, list[id].name);
    }
    free(list);
    return ok;
}
