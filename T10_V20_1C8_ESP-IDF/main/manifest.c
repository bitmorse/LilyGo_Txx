#include "manifest.h"

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
#define MAX_FILES 40      // bounds the static scan buffer (DRAM is tight: no PSRAM,
                          // and WiFi+BLE+LVGL already claim most of it). The manifest
                          // JSON buffer in filesrv.c is sized to hold all MAX_FILES.
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

void manifest_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();   // idempotent
}

void manifest_precache_hold(bool hold) { s_precache_hold = hold; }

static void lock(void)   { assert(s_lock); xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

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

int manifest_build_json(char *out, int cap)
{
    assert(out && cap > 64);
    unsigned long long freeb = 0;
    uint64_t total = 0, avail = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &avail) == ESP_OK)
        freeb = avail;

    lock();
    int n = scan(s_files, MAX_FILES);
    int off = snprintf(out, cap, "{\"v\":1,\"free_bytes\":%llu,\"files\":[", freeb);
    if (off < 0 || off >= cap) { unlock(); return -1; }         // header didn't fit

    for (int i = 0; i < n; i++) {
        char hex[65] = {0};
        bool have_hash = sha256_read_sidecar(s_files[i].name, hex);  // fast; no compute

        char iso[24];
        struct tm tmv;
        gmtime_r(&s_files[i].mtime, &tmv);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);

        // One entry needs at most ~200 bytes; stop cleanly (valid JSON) rather than
        // overflow. A truncated manifest beats a 500 -- the client still syncs what
        // it sees and re-lists later.
        char ent[288];
        int m = snprintf(ent, sizeof(ent),
            "%s{\"id\":%d,\"name\":\"%s\",\"mime\":\"%s\",\"bytes\":%ld,"
            "\"created_at\":\"%s\"%s%s%s}",
            i ? "," : "", i, s_files[i].name, mime_for(s_files[i].name),
            s_files[i].size, iso,
            have_hash ? ",\"sha256\":\"" : "", have_hash ? hex : "",
            have_hash ? "\"" : "");
        if (m < 0 || off + m >= cap - 2) break;                // leave room for "]}"
        memcpy(out + off, ent, m);
        off += m;
    }
    unlock();

    out[off++] = ']';
    out[off++] = '}';
    out[off]   = '\0';
    return off;
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
    char names[MAX_FILES][NAME_MAX_];          // MAX_FILES*NAME_MAX_ = 2560 B on stack
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
    }
    unlock();
    return ok;
}
