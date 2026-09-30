// ============================================================
// SigDash - data history storage (see sig_store.h)
// ============================================================

#include "sig_store.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <FFat.h>
#include <LittleFS.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Waveshare ESP32-S3-Touch-LCD-4.3B: TF card on SPI, CS through CH422G EXIO4
#define SD_MOSI 11
#define SD_CLK  12
#define SD_MISO 13
#define SD_SS   -1

#define STORE_DIR       "/sigdash"
#define DAYS_FILE STORE_DIR "/days.bin"

static fs::FS *fsys = nullptr;
static SemaphoreHandle_t mux = nullptr;
static StoreInfo info;
static bool is_sd = false, is_ffat = false;

#define LOCK()   xSemaphoreTake(mux, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mux)


// ============================================================
// helpers
// ============================================================

int32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

uint32_t date_yyyymmdd(time_t t)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}

static void qfile_name(char *buf, size_t n, int yyyymm)
{
    snprintf(buf, n, STORE_DIR "/q%06d.bin", yyyymm);
}

static int yyyymm_of(time_t t)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    return (tmv.tm_year + 1900) * 100 + tmv.tm_mon + 1;
}

static int months_index(int yyyymm) { return (yyyymm / 100) * 12 + (yyyymm % 100) - 1; }

static bool copy_file(fs::FS &from, fs::FS &to, const char *path)
{
    File a = from.open(path, FILE_READ);
    if (!a) return false;
    File b = to.open(path, FILE_WRITE);
    if (!b) { a.close(); return false; }
    uint8_t buf[512];
    size_t n;
    while ((n = a.read(buf, sizeof(buf))) > 0) b.write(buf, n);
    a.close();
    b.close();
    return true;
}


// ============================================================
// init
// ============================================================

bool store_begin(bool sd_cs_ready)
{
    mux = xSemaphoreCreateMutex();
    memset(&info, 0, sizeof(info));
    strcpy(info.medium, "-");

    bool flash_ok = false;
    fs::FS *flash = nullptr;
    if (FFat.begin(true)) {
        flash = &FFat;
        flash_ok = true;
        is_ffat = true;
    } else if (LittleFS.begin(true)) {
        flash = &LittleFS;
        flash_ok = true;
    }

    bool sd_ok = false;
    if (sd_cs_ready) {
        SPI.setHwCs(false);
        SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_SS);
        if (SD.begin(SD_SS) && SD.cardType() != CARD_NONE) sd_ok = true;
    }

    if (sd_ok) {
        fsys = &SD;
        is_sd = true;
        strcpy(info.medium, "SD");
        // first start with a card: take over what was collected in flash
        if (!SD.exists(DAYS_FILE) && flash_ok && flash->exists(STORE_DIR)) {
            SD.mkdir(STORE_DIR);
            File d = flash->open(STORE_DIR);
            File f;
            int n = 0;
            while (d && (f = d.openNextFile())) {
                char path[64];
                snprintf(path, sizeof(path), STORE_DIR "/%s", strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name());
                f.close();
                if (copy_file(*flash, SD, path)) n++;
            }
            Serial.printf("[STORE] copied %d files from flash to SD\n", n);
        }
    } else if (flash_ok) {
        fsys = flash;
        strcpy(info.medium, "Flash");
    } else {
        Serial.println("[STORE] no SD card and no data partition - history disabled");
        Serial.println("[STORE] Arduino IDE: Tools > Partition Scheme > 16M Flash (3MB APP/9.9MB FATFS)");
        return false;
    }

    if (!fsys->exists(STORE_DIR)) fsys->mkdir(STORE_DIR);
    info.ok = true;
    Serial.printf("[STORE] using %s\n", info.medium);
    store_refresh_info();
    return true;
}


// ============================================================
// write
// ============================================================

static void retention_cleanup(int current_yyyymm)
{
    File d = fsys->open(STORE_DIR);
    if (!d) return;
    int now_i = months_index(current_yyyymm);
    char victims[8][40];
    int nv = 0;
    File f;
    while ((f = d.openNextFile()) && nv < 8) {
        const char *name = strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name();
        int ym;
        if (sscanf(name, "q%6d.bin", &ym) == 1 && now_i - months_index(ym) >= STORE_KEEP_MONTHS)
            snprintf(victims[nv++], 40, STORE_DIR "/%s", name);
        f.close();
    }
    d.close();
    for (int i = 0; i < nv; i++) {
        fsys->remove(victims[i]);
        Serial.printf("[STORE] removed old %s\n", victims[i]);
    }
}

bool store_append_quarter(const QRec &q)
{
    if (!fsys) return false;
    char path[40];
    int ym = yyyymm_of((time_t)q.ts);
    qfile_name(path, sizeof(path), ym);

    uint32_t t0 = millis();
    LOCK();
    bool is_new = !fsys->exists(path);
    File f = fsys->open(path, FILE_APPEND);
    bool ok = f && f.write((const uint8_t *)&q, sizeof(q)) == sizeof(q);
    if (f) f.close();
    if (ok && is_new) retention_cleanup(ym);
    if (ok) info.used_bytes += sizeof(q);          // estimate - no free-space scan here
    UNLOCK();
    Serial.printf("[STORE] quarter %s on %s, %u ms\n", ok ? "saved" : "write FAILED", info.medium, (unsigned)(millis() - t0));
    return ok;
}

bool store_append_day(const DayRec &d)
{
    if (!fsys) return false;
    LOCK();
    File f = fsys->open(DAYS_FILE, FILE_APPEND);
    bool ok = f && f.write((const uint8_t *)&d, sizeof(d)) == sizeof(d);
    if (f) f.close();
    if (ok) {
        info.days++;
        if (!info.first_date) info.first_date = d.date;
    }
    UNLOCK();
    Serial.printf("[STORE] day %u %s\n", (unsigned)d.date, ok ? "saved" : "write FAILED");
    store_refresh_info();                          // once a day is fine
    return ok;
}

// ------------------------------------------------------------
// small state files on the SD card
// ------------------------------------------------------------
bool store_is_sd() { return fsys && is_sd; }

bool store_write_blob(const char *name, const void *data, size_t len)
{
    if (!store_is_sd()) return false;
    char tmp[48], dst[48];
    snprintf(tmp, sizeof(tmp), STORE_DIR "/%s.new", name);
    snprintf(dst, sizeof(dst), STORE_DIR "/%s", name);
    LOCK();
    File f = fsys->open(tmp, FILE_WRITE);
    bool ok = f && f.write((const uint8_t *)data, len) == len;
    if (f) f.close();
    if (ok) {
        fsys->remove(dst);
        ok = fsys->rename(tmp, dst);
    }
    UNLOCK();
    return ok;
}

bool store_read_blob(const char *name, void *data, size_t len)
{
    if (!store_is_sd()) return false;
    char path[48];
    bool ok = false;
    LOCK();
    for (int k = 0; k < 2 && !ok; k++) {           // after a power cut the .new file may be the only one
        snprintf(path, sizeof(path), k == 0 ? STORE_DIR "/%s" : STORE_DIR "/%s.new", name);
        File f = fsys->open(path, FILE_READ);
        if (f) {
            ok = f.size() == len && f.read((uint8_t *)data, len) == len;
            f.close();
        }
    }
    UNLOCK();
    return ok;
}


// ============================================================
// read (chunked: the store is locked only while a chunk is read,
// callbacks run unlocked, so they may send data over the network)
// ============================================================

#define CHUNK 16

template <typename T>
static int read_file_chunked(const char *path, bool (*accept)(const T &, void *), void (*cb)(const T &, void *),
                             void *ctx, void *actx)
{
    static_assert(sizeof(T) * CHUNK <= 2048, "chunk too big");
    T buf[CHUNK];
    size_t pos = 0;
    int count = 0;
    for (;;) {
        LOCK();
        File f = fsys->open(path, FILE_READ);
        if (!f) { UNLOCK(); break; }
        f.seek(pos);
        size_t got = f.read((uint8_t *)buf, sizeof(buf));
        f.close();
        UNLOCK();

        size_t n = got / sizeof(T);
        if (n == 0) break;
        pos += n * sizeof(T);
        for (size_t i = 0; i < n; i++) {
            if (accept(buf[i], actx)) {
                cb(buf[i], ctx);
                count++;
            }
        }
        if (n < CHUNK) break;
    }
    return count;
}

struct DayRange { uint32_t from, to; };
struct TsRange  { uint32_t from, to; };

static bool accept_day(const DayRec &d, void *a)
{
    DayRange *r = (DayRange *)a;
    return d.date >= r->from && d.date <= r->to;
}

static bool accept_q(const QRec &q, void *a)
{
    TsRange *r = (TsRange *)a;
    return q.ts >= r->from && q.ts <= r->to;
}

int store_read_days(uint32_t from_date, uint32_t to_date, day_cb_t cb, void *ctx)
{
    if (!fsys) return 0;
    DayRange r = {from_date, to_date};
    return read_file_chunked<DayRec>(DAYS_FILE, accept_day, cb, ctx, &r);
}

int store_read_quarters(uint32_t from_ts, uint32_t to_ts, quarter_cb_t cb, void *ctx)
{
    if (!fsys) return 0;
    TsRange r = {from_ts, to_ts};
    int count = 0;
    int m0 = months_index(yyyymm_of((time_t)from_ts));
    int m1 = months_index(yyyymm_of((time_t)to_ts));
    for (int m = m0; m <= m1; m++) {
        char path[40];
        qfile_name(path, sizeof(path), (m / 12) * 100 + (m % 12) + 1);
        count += read_file_chunked<QRec>(path, accept_q, cb, ctx, &r);
    }
    return count;
}


// ============================================================
// info
// ============================================================

void store_refresh_info()
{
    if (!fsys) return;
    StoreInfo n;
    LOCK();
    n = info;
    if (is_sd) {
        n.total_bytes = SD.totalBytes();
        n.used_bytes = SD.usedBytes();
    } else if (is_ffat) {
        n.total_bytes = FFat.totalBytes();
        n.used_bytes = FFat.totalBytes() - FFat.freeBytes();
    } else {
        n.total_bytes = LittleFS.totalBytes();
        n.used_bytes = LittleFS.usedBytes();
    }
    File f = fsys->open(DAYS_FILE, FILE_READ);
    if (f) {
        n.days = f.size() / sizeof(DayRec);
        DayRec d;
        if (f.read((uint8_t *)&d, sizeof(d)) == sizeof(d)) n.first_date = d.date;
        f.close();
    }
    info = n;
    UNLOCK();
}

void store_info(StoreInfo &out)
{
    if (!mux) { memset(&out, 0, sizeof(out)); strcpy(out.medium, "-"); return; }
    LOCK();
    out = info;
    UNLOCK();
}
