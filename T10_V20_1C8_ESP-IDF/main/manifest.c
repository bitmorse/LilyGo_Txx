#include "manifest.h"
#include "manifest_filter.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <assert.h>
#include "esp_vfs_fat.h"
#include "mbedtls/sha256.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "manifest";

#define MOUNT     "/sdcard"
#define MAX_FILES 96      // bounds the static scan buffer (DRAM is tight: no PSRAM,
                          // and WiFi+BLE+LVGL already claim most of it). The manifest
                          // is streamed (no per-request buffer), so this is the only
                          // ceiling; the precache task stack in filesrv.c holds a
                          // names[MAX_FILES][NAME_MAX_] array -- grow it in step.
#define NAME_MAX_ 64

typedef struct {
    char   name[NAME_MAX_];
    long   size;
    time_t mtime;
} entry_t;

// A single, mutex-guarded scan buffer shared by every entry point. NO heap in the
// manifest path (a malloc here failed in low-heap AP+BLE mode -> /manifest 500).
// Every public function locks, scans into s_files, does its FAST work (never a full
// file read), and unlocks -- so the lock is held only for quick directory scans,
// never during hashing or streaming. Created once in manifest_init() before any
// task can call in (single-threaded at that point).
static entry_t          s_files[MAX_FILES];
static SemaphoreHandle_t s_lock;

// Set true by the HTTP file handler while a transfer is in progress. The background
// precache polls it and yields the SD bus, so a client download runs at full speed
// instead of contending with sha256 hashing.
static volatile bool s_precache_hold;

// Cached directory scan (this SD card is ~0.3 s per stat, so re-scanning per request
// is too slow). Valid for a whole serving session: files don't change mid-session
// (recording is refused while serving), only a delete mutates them -- which
// invalidates it. Held under s_lock like s_files. -1 = invalid, must re-scan.
static int s_scan_n = -1;

static void scan_invalidate(void) { s_scan_n = -1; }

void manifest_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();   // idempotent
    s_scan_n = -1;                                    // fresh session -> re-scan once
}

void manifest_precache_hold(bool hold) { s_precache_hold = hold; }

static void lock(void)   { assert(s_lock); xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

// --- helpers ----------------------------------------------------------------

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
// Result is cached in s_files (see s_scan_n): callers all pass s_files, so a valid
// cache is reused instead of re-hitting the slow SD. Call under s_lock.
static int scan(entry_t *list, int max)
{
    if (list == s_files && s_scan_n >= 0) return s_scan_n;   // cached this session

    DIR *d = opendir(MOUNT);
    if (!d) { ESP_LOGW(TAG, "opendir(%s) failed", MOUNT); return 0; }

    int n = 0;
    struct dirent *de;
    while (n < max && (de = readdir(d)) != NULL) {
        if (de->d_type == DT_DIR) continue;
        if (!manifest_include_name(de->d_name)) continue;   // .mcap recordings only
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
    if (n == max)                                       // don't cap silently
        ESP_LOGW(TAG, "manifest capped at %d files; extra .mcap files hidden", max);
    closedir(d);
    qsort(list, n, sizeof(entry_t), cmp_name);          // stable id ordering
    if (list == s_files) s_scan_n = n;                  // cache for the session
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

    uint8_t buf[2048];                                  // stack (precache task only)
    ssize_t r;
    while (remaining > 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
        // Yield the SD bus to an active client download: this only runs on the
        // background precache task, so pausing here costs nothing the user sees.
        while (s_precache_hold) vTaskDelay(pdMS_TO_TICKS(100));
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

// Append `src` (len m) to the fixed `buf`, flushing to `sink` first if it wouldn't
// fit. Returns 0, or -1 if the sink aborted. `*plen` tracks bytes buffered.
static int emit(manifest_sink_fn sink, void *ctx, char *buf, int cap,
                int *plen, const char *src, int m)
{
    assert(sink && buf && plen && m >= 0 && m <= cap);
    if (*plen + m > cap) {                        // flush what we have, then append
        if (sink(ctx, buf, *plen) != 0) return -1;
        *plen = 0;
    }
    memcpy(buf + *plen, src, m);
    *plen += m;
    return 0;
}

int manifest_stream_json(manifest_sink_fn sink, void *ctx)
{
    assert(sink);
    unsigned long long freeb = 0;
    uint64_t total = 0, avail = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &avail) == ESP_OK)
        freeb = avail;

    char buf[1024];                               // chunk accumulator (stack)
    int len = 0, rc = 0;

    lock();
    int n = scan(s_files, MAX_FILES);

    char hdr[64];
    int h = snprintf(hdr, sizeof(hdr), "{\"v\":1,\"free_bytes\":%llu,\"files\":[", freeb);
    if (h < 0 || h >= (int)sizeof(hdr)) { unlock(); return -1; }
    rc = emit(sink, ctx, buf, sizeof(buf), &len, hdr, h);

    // NOTE: the manifest deliberately omits sha256 -- reading a .s256 sidecar per file
    // was ~1.6 s/file on this SD card (25 s for a dozen files -> the app's 10 s timeout).
    // The app verifies integrity from the /file ETag (which the device sets from the
    // sidecar, one read at download time) instead. Keeps /manifest cheap + scalable.
    for (int i = 0; i < n && rc == 0; i++) {
        char iso[24];
        struct tm tmv;
        gmtime_r(&s_files[i].mtime, &tmv);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);

        char ent[288];                            // one entry needs at most ~160 B
        int m = snprintf(ent, sizeof(ent),
            "%s{\"id\":%d,\"name\":\"%s\",\"mime\":\"%s\",\"bytes\":%ld,"
            "\"created_at\":\"%s\"}",
            i ? "," : "", i, s_files[i].name, mime_for(s_files[i].name),
            s_files[i].size, iso);
        if (m < 0 || m >= (int)sizeof(ent)) continue;   // never expected; skip entry
        rc = emit(sink, ctx, buf, sizeof(buf), &len, ent, m);
    }

    if (rc == 0) rc = emit(sink, ctx, buf, sizeof(buf), &len, "]}", 2);
    if (rc == 0 && len > 0 && sink(ctx, buf, len) != 0) rc = -1;   // final flush
    unlock();
    return rc;
}

bool manifest_path_for_id(int id, char *path, int cap)
{
    lock();
    int n = scan(s_files, MAX_FILES);
    bool ok = (id >= 0 && id < n);
    if (ok) snprintf(path, cap, "%s/%s", MOUNT, s_files[id].name);
    unlock();
    return ok;
}

// ETag lookup for /file: sidecar-only, NEVER computes (a full-file read here would
// stall the body before a single byte streams -> client timeout). Returns false if
// the sha256 hasn't been cached by the background precache yet -> ETag is omitted.
bool manifest_sha256_for_id(int id, char *hex64, int cap)
{
    if (cap < 65) return false;
    char name[NAME_MAX_];
    lock();
    int n = scan(s_files, MAX_FILES);
    bool ok = (id >= 0 && id < n);
    if (ok) memcpy(name, s_files[id].name, NAME_MAX_);
    unlock();
    return ok && sha256_read_sidecar(name, hex64);
}

void manifest_precache(void)
{
    // Snapshot the names under the lock, then hash OUTSIDE it (hashing is slow and
    // must not hold the scan lock, nor block a client download -- see the hold flag).
    // On the precache task's own stack (bumped to 8 KB), not BSS -- DRAM is scarce.
    char names[MAX_FILES][NAME_MAX_];          // MAX_FILES*NAME_MAX_ = 6144 B on stack
    lock();
    int n = scan(s_files, MAX_FILES);
    for (int i = 0; i < n; i++) memcpy(names[i], s_files[i].name, NAME_MAX_);
    unlock();

    char hex[65];
    for (int i = 0; i < n; i++) {
        while (s_precache_hold) vTaskDelay(pdMS_TO_TICKS(200));  // yield to a transfer
        if (sha256_read_sidecar(names[i], hex)) continue;       // already cached
        int64_t t0 = esp_timer_get_time();
        if (sha256_cached(names[i], hex))                       // computes + caches
            ESP_LOGI(TAG, "sha256 %s in %lld ms", names[i],
                     (esp_timer_get_time() - t0) / 1000);
    }
    ESP_LOGI(TAG, "precache done (%d files)", n);
}

bool manifest_delete_id(int id)
{
    char path[300];
    lock();
    int n = scan(s_files, MAX_FILES);
    bool ok = (id >= 0 && id < n);
    if (ok) {
        snprintf(path, sizeof(path), "%s/%s", MOUNT, s_files[id].name);
        remove(path);
        snprintf(path, sizeof(path), "%s/%s.s256", MOUNT, s_files[id].name);
        remove(path);                                    // best-effort sidecar
        ESP_LOGI(TAG, "deleted id %d (%s)", id, s_files[id].name);
        scan_invalidate();                               // files changed -> re-scan
    }
    unlock();
    return ok;
}
