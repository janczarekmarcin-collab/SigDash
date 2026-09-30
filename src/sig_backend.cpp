// ============================================================
// SigDash backend - see sig_backend.h
// ============================================================

#include "sig_backend.h"
#include "sig_store.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

// ---------------- configuration ----------------
#define SIG_PLANT_ID        247
#define SIG_PORT            502
#define POLL_PERIOD_MS      5000
#define MB_TIMEOUT_MS       1500
#define DISCOVER_TIMEOUT_MS 150
#define PRICE_RETRY_MS      (10UL * 60UL * 1000UL)
#define PRICE_REFRESH_MS    (60UL * 60UL * 1000UL)
#define SAVE_PERIOD_MS      (60UL * 60UL * 1000UL)   // fallback only - normally saved with each quarter record
#define HTTPS_MIN_INTERNAL  56000                    // TLS needs ~45 KB of internal RAM
#define HTTPS_MIN_BLOCK     36000
#define TZ_POLAND           "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_CET              TZ_POLAND
#define TZ_EET              "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define TZ_WET              "WET0WEST,M3.5.0/1,M10.5.0"
#define TZ_IE               "GMT0IST,M3.5.0/1,M10.5.0"

const PriceZone SIG_ZONES[] = {
    {"PL",              "Polska",              "Poland",            TZ_CET},
    {"DE-LU",           "Niemcy / Luks.",      "Germany / Lux.",    TZ_CET},
    {"AT",              "Austria",             "Austria",           TZ_CET},
    {"CH",              "Szwajcaria",          "Switzerland",       TZ_CET},
    {"CZ",              "Czechy",              "Czechia",           TZ_CET},
    {"SK",              "Słowacja",            "Slovakia",          TZ_CET},
    {"HU",              "Węgry",               "Hungary",           TZ_CET},
    {"SI",              "Słowenia",            "Slovenia",          TZ_CET},
    {"HR",              "Chorwacja",           "Croatia",           TZ_CET},
    {"RS",              "Serbia",              "Serbia",            TZ_CET},
    {"ME",              "Czarnogóra",          "Montenegro",        TZ_CET},
    {"NL",              "Holandia",            "Netherlands",       TZ_CET},
    {"BE",              "Belgia",              "Belgium",           TZ_CET},
    {"FR",              "Francja",             "France",            TZ_CET},
    {"ES",              "Hiszpania",           "Spain",             TZ_CET},
    {"PT",              "Portugalia",          "Portugal",          TZ_WET},
    {"IE(SEM)",         "Irlandia",            "Ireland",           TZ_IE},
    {"IT-North",        "Włochy Płn.",         "Italy North",       TZ_CET},
    {"IT-Centre-North", "Włochy Śr.-Płn.",     "Italy Centre-N.",   TZ_CET},
    {"IT-Centre-South", "Włochy Śr.-Płd.",     "Italy Centre-S.",   TZ_CET},
    {"IT-South",        "Włochy Płd.",         "Italy South",       TZ_CET},
    {"IT-Sicily",       "Sycylia",             "Sicily",            TZ_CET},
    {"IT-Sardinia",     "Sardynia",            "Sardinia",          TZ_CET},
    {"DK1",             "Dania Zach.",         "Denmark West",      TZ_CET},
    {"DK2",             "Dania Wsch.",         "Denmark East",      TZ_CET},
    {"NO1",             "Norwegia NO1",        "Norway NO1",        TZ_CET},
    {"NO2",             "Norwegia NO2",        "Norway NO2",        TZ_CET},
    {"NO3",             "Norwegia NO3",        "Norway NO3",        TZ_CET},
    {"NO4",             "Norwegia NO4",        "Norway NO4",        TZ_CET},
    {"NO5",             "Norwegia NO5",        "Norway NO5",        TZ_CET},
    {"SE1",             "Szwecja SE1",         "Sweden SE1",        TZ_CET},
    {"SE2",             "Szwecja SE2",         "Sweden SE2",        TZ_CET},
    {"SE3",             "Szwecja SE3",         "Sweden SE3",        TZ_CET},
    {"SE4",             "Szwecja SE4",         "Sweden SE4",        TZ_CET},
    {"FI",              "Finlandia",           "Finland",           TZ_EET},
    {"EE",              "Estonia",             "Estonia",           TZ_EET},
    {"LV",              "Łotwa",               "Latvia",            TZ_EET},
    {"LT",              "Litwa",               "Lithuania",         TZ_EET},
    {"RO",              "Rumunia",             "Romania",           TZ_EET},
    {"BG",              "Bułgaria",            "Bulgaria",          TZ_EET},
    {"GR",              "Grecja",              "Greece",            TZ_EET},
};
const int SIG_ZONE_COUNT = sizeof(SIG_ZONES) / sizeof(SIG_ZONES[0]);

const char *sig_zone_tz(const Settings &s)
{
    if (s.price_src == SRC_RCE) return TZ_POLAND;
    if (s.zone < SIG_ZONE_COUNT) return SIG_ZONES[s.zone].tz;
    return TZ_POLAND;
}

// ---------------- shared state ----------------
static SemaphoreHandle_t g_mux;
static SigData   g_data;
static PriceData g_prices;
static MoneyData g_money;
static NetStatus g_net;
static Settings  g_set;
static uint8_t   g_soc_hist[96];      // today's SOC [%] per local quarter, 255 = none

// statistics of the current day (for the day record / export)
struct DayStats {
    uint8_t soc_min, soc_max;
    float   pv_peak_kw, imp_peak_kw;
    double  price_sum, price_hours;       // time-weighted market price
};
static DayStats g_dstat;

// the 15-minute interval being collected
struct QAcc {
    bool     any;
    uint32_t ts;
    double   pv, load, imp, exp_, bc, bd, saved, expv, impc, dep_out;
    float    pm, pe;
    bool     pok;
    uint8_t  soc;
};
static QAcc g_qacc;
static int       g_soc_day = 0;       // yyyymmdd of g_soc_hist

// requests (written by UI, consumed by net task)
static volatile bool rq_scan = false;
static volatile bool rq_connect = false;
static volatile bool rq_discover = false;
static volatile bool rq_set_ip = false;
static volatile bool rq_reset_money = false;
static volatile bool rq_save_settings = false;
static volatile bool rq_save_money = false;
static char rq_ssid[33], rq_pass[65], rq_ip[16];

static Preferences prefs;

#define LOCK()   xSemaphoreTake(g_mux, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_mux)


// ============================================================
// SMALL HELPERS
// ============================================================

// Large buffers go to PSRAM - internal RAM is needed by WiFi, TLS and the display.
static void *ps_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(n);
}

static size_t internal_free()
{
#ifdef ESP_PLATFORM
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return 1 << 20;
#endif
}

static size_t internal_largest()
{
#ifdef ESP_PLATFORM
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return 1 << 20;
#endif
}

void sig_mem_report(const char *tag)
{
#ifdef ESP_PLATFORM
    Serial.printf("[MEM] %s | internal free %u, min %u, largest %u | psram free %u\n", tag,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    (void)tag;
#endif
}

static inline const char *L(const char *pl, const char *en)
{
    return g_set.lang == LANG_EN ? en : pl;
}

static void set_msg(const char *fmt, ...)
{
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    LOCK();
    strlcpy(g_net.msg, b, sizeof(g_net.msg));
    UNLOCK();
    Serial.printf("[SIG] %s\n", b);
}

static void set_sig_state(SigState s)
{
    LOCK();
    g_net.sig_state = s;
    UNLOCK();
}

static inline uint16_t U16(const uint16_t *r, int i) { return r[i]; }
static inline int16_t  S16(const uint16_t *r, int i) { return (int16_t)r[i]; }
static inline uint32_t U32(const uint16_t *r, int i) { return ((uint32_t)r[i] << 16) | r[i + 1]; }
static inline int32_t  S32(const uint16_t *r, int i) { return (int32_t)U32(r, i); }
static inline uint64_t U64(const uint16_t *r, int i)
{
    return ((uint64_t)r[i] << 48) | ((uint64_t)r[i + 1] << 32) | ((uint64_t)r[i + 2] << 16) | r[i + 3];
}

const char *sig_ems_mode_text(uint16_t m, uint8_t lang)
{
    bool en = lang == LANG_EN;
    switch (m) {
    case 0: return en ? "Max self-consumption" : "Maks. autokonsumpcja";
    case 1: return en ? "AI mode" : "Tryb AI";
    case 2: return en ? "Time of use (TOU)" : "Taryfa czasowa (TOU)";
    case 5: return en ? "Full feed-in" : "Pełna sprzedaż";
    case 7: return en ? "Remote EMS" : "Zdalny EMS";
    case 9: return en ? "Custom" : "Własny";
    default: return "?";
    }
}

const char *sig_run_state_text(uint16_t s, uint8_t lang)
{
    bool en = lang == LANG_EN;
    switch (s) {
    case 0: return en ? "Standby" : "Czuwanie";
    case 1: return en ? "Running" : "Praca";
    case 2: return en ? "Fault" : "Awaria";
    case 3: return en ? "Shutdown" : "Wyłączony";
    case 7: return en ? "Environment abnormal" : "Anomalia środowiska";
    default: return "?";
    }
}

const char *sig_currency(uint8_t c)
{
    switch (c) {
    case CUR_EUR: return "€";
    case CUR_GBP: return "£";
    case CUR_USD: return "$";
    default:      return "zł";
    }
}


// ============================================================
// SETTINGS / PERSISTENCE
// ============================================================

static void settings_load()
{
    prefs.begin("sigdash", true);
    strlcpy(g_set.ssid, prefs.getString("ssid", "").c_str(), sizeof(g_set.ssid));
    strlcpy(g_set.pass, prefs.getString("pass", "").c_str(), sizeof(g_set.pass));
    strlcpy(g_set.sig_ip, prefs.getString("sigip", "").c_str(), sizeof(g_set.sig_ip));
    g_set.inv_id       = prefs.getUChar("invid", 1);
    g_set.import_price = prefs.getFloat("price", 1.10f);
    g_set.export_coef  = prefs.getFloat("coef", 1.23f);
    g_set.neg_as_zero  = prefs.getBool("neg0", true);
    g_set.lang         = prefs.getUChar("lang", LANG_PL);
    g_set.earn_mode    = prefs.getUChar("emode", EARN_TOTAL);
    if (prefs.isKey("psrc2")) {
        g_set.price_src = prefs.getUChar("psrc2", SRC_RCE);
        g_set.price_res = prefs.getUChar("pres", RES_HOUR);
    } else {                                   // migrate v1.1
        uint8_t old = prefs.getUChar("psrc", 0);
        g_set.price_src = (old == 2) ? SRC_FIXED : SRC_RCE;
        g_set.price_res = (old == 1) ? RES_QUARTER : RES_HOUR;
    }
    g_set.zone         = prefs.getUChar("zone", SIG_ZONE_PL);
    if (g_set.zone >= SIG_ZONE_COUNT) g_set.zone = SIG_ZONE_PL;
    g_set.fx_rate      = prefs.getFloat("fx", 4.25f);
    g_set.fixed_export_price = prefs.getFloat("fixp", 0.30f);
    g_set.energy_price = prefs.getFloat("eprice", 0.62f);
    g_set.currency     = prefs.getUChar("cur", CUR_PLN);
    g_set.lcd_clk      = sig_lcd_clk_idx(prefs.getUChar("lcdmhz", 16));   // old "lcdclk" key is ignored
    g_set.lcd_bb       = prefs.getUChar("lcdbb", 1);
    if (g_set.lcd_bb > 2) g_set.lcd_bb = 1;
    prefs.end();
}

static void settings_save()
{
    Settings s;
    LOCK();
    s = g_set;
    UNLOCK();
    prefs.begin("sigdash", false);
    prefs.putString("ssid", s.ssid);
    prefs.putString("pass", s.pass);
    prefs.putString("sigip", s.sig_ip);
    prefs.putUChar("invid", s.inv_id);
    prefs.putFloat("price", s.import_price);
    prefs.putFloat("coef", s.export_coef);
    prefs.putBool("neg0", s.neg_as_zero);
    prefs.putUChar("lang", s.lang);
    prefs.putUChar("emode", s.earn_mode);
    prefs.putUChar("psrc2", s.price_src);
    prefs.putUChar("pres", s.price_res);
    prefs.putUChar("zone", s.zone);
    prefs.putFloat("fx", s.fx_rate);
    prefs.putFloat("fixp", s.fixed_export_price);
    prefs.putFloat("eprice", s.energy_price);
    prefs.putUChar("cur", s.currency);
    prefs.putUChar("lcdmhz", sig_lcd_clk_mhz(s.lcd_clk));
    prefs.putUChar("lcdbb", s.lcd_bb);
    prefs.end();
}

static int  money_day = 0;     // yyyymmdd of g_money.today
static int  money_mon = 0;     // yyyymm   of g_money.month

// All counters in one record: one NVS write instead of a dozen
// (every flash write stalls PSRAM -> RGB display glitch).
struct MoneyBlob {
    uint32_t magic;
    uint32_t seq;              // incremented on every save; the newer copy wins
    int32_t  day, mon, socday;
    Money    today, month, total;
    double   yday, dep, ydep;
    DayStats ds;
    uint8_t  soch[96];
    uint32_t crc;
};
#define MONEY_MAGIC   0x53444D31u   // "SDM1"
#define MONEY_FILE    "money.bin"
#define NVS_BACKUP_MS (6UL * 3600UL * 1000UL)   // with an SD card: NVS copy only every 6 h

static uint32_t money_seq = 0;
static uint32_t last_nvs_money = 0;

static uint32_t blob_crc(const MoneyBlob &b)
{
    const uint8_t *p = (const uint8_t *)&b;
    size_t n = offsetof(MoneyBlob, crc);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static bool blob_valid(const MoneyBlob &b) { return b.magic == MONEY_MAGIC && b.crc == blob_crc(b); }

static void blob_apply(const MoneyBlob &b)
{
    money_day = b.day;
    money_mon = b.mon;
    g_soc_day = b.socday;
    g_money.today = b.today;
    g_money.month = b.month;
    g_money.total = b.total;
    g_money.yesterday_earn = b.yday;
    g_money.deposit = b.dep;
    g_money.yesterday_dep = b.ydep;
    g_dstat = b.ds;
    memcpy(g_soc_hist, b.soch, sizeof(g_soc_hist));
    money_seq = b.seq;
}

static void money_load()
{
    static MoneyBlob nb, sb;          // ~400 B - keep off the stack
    bool have_nvs = false;

    prefs.begin("sigmoney", true);
    if (prefs.getBytesLength("blob") == sizeof(MoneyBlob)) {
        prefs.getBytes("blob", &nb, sizeof(nb));
        have_nvs = blob_valid(nb);
    }
    if (!have_nvs) {
        // v1.5 and older: separate keys
        money_day = prefs.getInt("day", 0);
        money_mon = prefs.getInt("mon", 0);
        if (prefs.getBytesLength("today") == sizeof(Money)) prefs.getBytes("today", &g_money.today, sizeof(Money));
        if (prefs.getBytesLength("month") == sizeof(Money)) prefs.getBytes("month", &g_money.month, sizeof(Money));
        if (prefs.getBytesLength("total") == sizeof(Money)) prefs.getBytes("total", &g_money.total, sizeof(Money));
        g_money.yesterday_earn = prefs.getDouble("yday", 0);
        g_money.deposit = prefs.getDouble("dep", 0);
        g_money.yesterday_dep = prefs.getDouble("ydep", 0);
        g_soc_day = prefs.getInt("socday", 0);
        if (prefs.getBytesLength("dstat") == sizeof(DayStats)) prefs.getBytes("dstat", &g_dstat, sizeof(DayStats));
        if (prefs.getBytesLength("soch") == sizeof(g_soc_hist)) prefs.getBytes("soch", g_soc_hist, sizeof(g_soc_hist));
    }
    prefs.end();

    bool have_sd = store_read_blob(MONEY_FILE, &sb, sizeof(sb)) && blob_valid(sb);
    if (have_sd && (!have_nvs || (int32_t)(sb.seq - nb.seq) >= 0)) {
        blob_apply(sb);
        Serial.printf("[SIG] counters loaded from SD (seq %u)\n", (unsigned)sb.seq);
    } else if (have_nvs) {
        blob_apply(nb);
        Serial.printf("[SIG] counters loaded from flash (seq %u)\n", (unsigned)nb.seq);
    }
}

// force_nvs: also write the internal-flash copy even if an SD card is used
static void money_save(bool force_nvs = false)
{
    static MoneyBlob b;
    memset(&b, 0, sizeof(b));
    LOCK();
    b.magic = MONEY_MAGIC;
    b.seq = ++money_seq;
    b.day = money_day;
    b.mon = money_mon;
    b.socday = g_soc_day;
    b.today = g_money.today;
    b.month = g_money.month;
    b.total = g_money.total;
    b.yday = g_money.yesterday_earn;
    b.dep = g_money.deposit;
    b.ydep = g_money.yesterday_dep;
    b.ds = g_dstat;
    memcpy(b.soch, g_soc_hist, sizeof(b.soch));
    UNLOCK();
    b.crc = blob_crc(b);

    uint32_t t0 = millis();
    bool sd_ok = store_write_blob(MONEY_FILE, &b, sizeof(b));
    bool nvs = !sd_ok || force_nvs || !last_nvs_money || millis() - last_nvs_money > NVS_BACKUP_MS;
    if (nvs) {
        prefs.begin("sigmoney", false);
        prefs.putBytes("blob", &b, sizeof(b));
        prefs.end();
        last_nvs_money = millis();
    }
    Serial.printf("[SIG] counters saved:%s%s, %u ms\n", sd_ok ? " SD" : "", nvs ? " flash" : "",
                  (unsigned)(millis() - t0));
}


// ============================================================
// MODBUS TCP CLIENT (function 0x04 - read input registers)
// ============================================================

class ModbusTcp {
public:
    bool connect(const IPAddress &a, uint32_t timeout_ms = 2000)
    {
        if (cli.connected() && a == ip) return true;
        cli.stop();
        ip = a;
        if (!cli.connect(a, SIG_PORT, (int32_t)timeout_ms)) return false;
        cli.setNoDelay(true);
        return true;
    }

    void stop() { cli.stop(); }
    bool connected() { return cli.connected(); }

    // 0 = OK, >0 = Modbus exception code, <0 = communication error
    int readInput(uint8_t unit, uint16_t addr, uint16_t cnt, uint16_t *out)
    {
        if (cnt == 0 || cnt > 125) return -10;
        if (!cli.connected()) return -1;

        while (cli.available()) cli.read();          // drop stale bytes

        uint16_t t = tid++;
        uint8_t q[12] = {
            (uint8_t)(t >> 8), (uint8_t)t, 0, 0, 0, 6, unit, 0x04,
            (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(cnt >> 8), (uint8_t)cnt
        };
        if (cli.write(q, sizeof(q)) != sizeof(q)) return -2;

        uint8_t h[9];
        if (!readN(h, 9, MB_TIMEOUT_MS)) return -3;
        if ((uint16_t)((h[0] << 8) | h[1]) != t) return -4;
        if (h[7] == 0x84) return h[8] ? h[8] : 0xFF;   // exception
        if (h[7] != 0x04) return -5;
        uint8_t bc = h[8];
        if (bc != cnt * 2) return -6;

        uint8_t buf[250];
        if (!readN(buf, bc, MB_TIMEOUT_MS)) return -3;
        for (int i = 0; i < cnt; i++) out[i] = ((uint16_t)buf[2 * i] << 8) | buf[2 * i + 1];
        return 0;
    }

private:
    bool readN(uint8_t *b, size_t n, uint32_t to)
    {
        uint32_t t0 = millis();
        size_t got = 0;
        while (got < n) {
            int a = cli.available();
            if (a > 0) {
                int r = cli.read(b + got, n - got);
                if (r > 0) got += r;
            } else {
                if (!cli.connected() || millis() - t0 > to) return false;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
        return true;
    }

    WiFiClient cli;
    IPAddress ip;
    uint16_t tid = 1;
};

static ModbusTcp mb;


// ============================================================
// POLLING - register blocks without address gaps
// ============================================================

struct Block {
    uint8_t  unit_plant;   // 1 = plant (247), 0 = inverter
    uint16_t addr;
    uint16_t cnt;
    bool     supported;
    uint32_t retry_ms;     // when to retry an unsupported block
};

enum { B_P1, B_P2, B_P3, B_P4, B_I1, B_I2, B_I3, B_COUNT };

static Block blocks[B_COUNT] = {
    {1, 30000, 73, true, 0},   // plant: time .. alarm5
    {1, 30083, 15, true, 0},   // plant: rated capacity, cut-offs, SOH, PV + load counters
    {1, 30200, 24, true, 0},   // plant: battery + grid counters
    {1, 30272, 15, true, 0},   // plant: PV daily, load power, cell temp (newer FW)
    {0, 30566, 44, true, 0},   // inverter: ESS daily, state, power, SOC, temps, alarms
    {0, 30620,  4, true, 0},   // inverter: battery temp/cell voltage min/max
    {0, 31000, 38, true, 0},   // inverter: grid V/I/f, PV strings, insulation
};

static uint16_t regs[125];
static int consecutive_fail = 0;

static void decode_block(int b, const uint16_t *r, SigData &d)
{
    switch (b) {
    case B_P1:
        d.sys_time       = U32(r, 0);
        d.tz_min         = S16(r, 2);
        d.ems_mode       = U16(r, 3);
        d.grid_sensor_ok = U16(r, 4);
        d.grid_kw        = S32(r, 5) / 1000.0f;
        d.on_off_grid    = U16(r, 9);
        d.soc            = U16(r, 14) / 10.0f;
        for (int i = 0; i < 4; i++) d.alarms[i] = U16(r, 27 + i);
        d.plant_kw       = S32(r, 31) / 1000.0f;
        d.pv_kw          = S32(r, 35) / 1000.0f;
        d.ess_kw         = S32(r, 37) / 1000.0f;
        d.ess_max_chg_kw = U32(r, 47) / 1000.0f;
        d.ess_max_dis_kw = U32(r, 49) / 1000.0f;
        d.run_state      = U16(r, 51);
        for (int i = 0; i < 3; i++) d.grid_ph_kw[i] = S32(r, 52 + 2 * i) / 1000.0f;
        d.ess_avail_chg_kwh = U32(r, 64) / 100.0f;
        d.ess_avail_dis_kwh = U32(r, 66) / 100.0f;
        d.alarms[4]      = U16(r, 72);
        break;

    case B_P2:
        d.ess_rated_kwh  = U32(r, 0) / 100.0f;
        d.chg_cutoff_soc = U16(r, 2) / 10.0f;
        d.dis_cutoff_soc = U16(r, 3) / 10.0f;
        d.soh            = U16(r, 4) / 10.0f;
        d.acc_pv         = U64(r, 5) / 100.0;
        d.acc_load       = U64(r, 11) / 100.0;
        d.have_acc_pv    = true;
        d.have_acc_load  = true;
        break;

    case B_P3:
        d.acc_bchg       = U64(r, 0) / 100.0;
        d.acc_bdis       = U64(r, 4) / 100.0;
        d.acc_imp        = U64(r, 16) / 100.0;
        d.acc_exp        = U64(r, 20) / 100.0;
        d.have_acc_batt  = true;
        d.have_acc_grid  = true;
        break;

    case B_P4:
        d.pv_today_reg   = U32(r, 0) / 100.0f;
        d.pv_yday_reg    = U32(r, 2) / 100.0f;
        d.have_pv_daily  = true;
        d.alarms[5]      = U16(r, 8);
        d.alarms[6]      = U16(r, 9);
        d.load_kw        = S32(r, 12) / 1000.0f;     // total load power
        d.load_from_reg  = true;
        d.plant_cell_temp = S16(r, 14) / 10.0f;
        break;

    case B_I1:
        d.ess_day_chg_kwh = U32(r, 0) / 100.0f;
        d.ess_day_dis_kwh = U32(r, 6) / 100.0f;
        d.inv_state      = U16(r, 12);
        d.inv_kw         = S32(r, 21) / 1000.0f;
        d.bat_temp_avg   = S16(r, 37) / 10.0f;
        d.cell_v_avg     = U16(r, 38) / 1000.0f;
        for (int i = 0; i < 5; i++) d.inv_alarms[i] = U16(r, 39 + i);
        break;

    case B_I2:
        d.bat_temp_max   = S16(r, 0) / 10.0f;
        d.bat_temp_min   = S16(r, 1) / 10.0f;
        d.cell_v_max     = U16(r, 2) / 1000.0f;
        d.cell_v_min     = U16(r, 3) / 1000.0f;
        break;

    case B_I3:
        d.grid_freq      = U16(r, 2) / 100.0f;
        d.inv_temp       = S16(r, 3) / 10.0f;
        for (int i = 0; i < 3; i++) {
            d.v_ph[i] = U32(r, 11 + 2 * i) / 100.0f;
            d.i_ph[i] = S32(r, 17 + 2 * i) / 100.0f;
        }
        d.pf             = U16(r, 23) / 1000.0f;
        d.mppt_count     = U16(r, 26);
        for (int i = 0; i < 4; i++) {
            d.pv_v[i] = S16(r, 27 + 2 * i) / 10.0f;
            d.pv_i[i] = S16(r, 28 + 2 * i) / 100.0f;
        }
        d.inv_pv_kw      = S32(r, 35) / 1000.0f;
        d.insulation_mohm = U16(r, 37) / 1000.0f;
        break;
    }
}

// returns true if the plant answered
static bool poll_once(uint8_t inv_id)
{
    SigData d;
    LOCK();
    d = g_data;
    UNLOCK();

    d.load_from_reg = false;
    bool plant_ok = false;
    bool inv_read = false;
    bool comm_err = false;

    for (int b = 0; b < B_COUNT; b++) {
        Block &bl = blocks[b];
        if (!bl.supported && millis() < bl.retry_ms) continue;

        uint8_t unit = bl.unit_plant ? SIG_PLANT_ID : inv_id;
        int rc = mb.readInput(unit, bl.addr, bl.cnt, regs);

        if (rc == 0) {
            decode_block(b, regs, d);
            bl.supported = true;
            if (bl.unit_plant) plant_ok = true;
            if (b == B_I1) inv_read = true;
        }
        else if (rc > 0) {
            // Modbus exception (e.g. 2 = illegal address: older firmware) - skip block for 10 min
            if (bl.supported) Serial.printf("[SIG] block %d (%u) exception %d - skipped\n", b, bl.addr, rc);
            bl.supported = false;
            bl.retry_ms = millis() + 10UL * 60UL * 1000UL;
        }
        else {
            Serial.printf("[SIG] block %d read error %d\n", b, rc);
            comm_err = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    d.inv_ok = inv_read;

    if (!d.load_from_reg) {
        // power balance: load = PV + grid - battery(charging +)
        d.load_kw = d.pv_kw + d.grid_kw - d.ess_kw;
        if (d.load_kw < 0) d.load_kw = 0;
    }
    if (!blocks[B_P3].supported) { d.have_acc_grid = false; d.have_acc_batt = false; }
    if (!blocks[B_P2].supported) { d.have_acc_pv = false; d.have_acc_load = false; }
    if (!blocks[B_P4].supported) d.have_pv_daily = false;

    if (plant_ok && !comm_err) {
        d.valid = true;
        d.updated_ms = millis();
        d.poll_count++;
        LOCK();
        g_data = d;
        UNLOCK();
        return true;
    }
    return false;
}


// ============================================================
// DISCOVERY - find Sigenergy on the local /24 network (port 502)
// ============================================================

static bool probe_ip(const IPAddress &ip, uint32_t tcp_timeout)
{
    WiFiClient c;
    if (!c.connect(ip, SIG_PORT, (int32_t)tcp_timeout)) return false;
    c.stop();

    // port open - check that it really is a Sigenergy plant (unit 247, reg 30000)
    ModbusTcp t;
    if (!t.connect(ip, 1500)) return false;
    uint16_t r[3];
    int rc = t.readInput(SIG_PLANT_ID, 30000, 3, r);
    t.stop();
    return rc == 0;
}

static bool discover()
{
    set_sig_state(SIG_SEARCHING);
    IPAddress me = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    set_msg(L("Szukam Sigenergy w sieci %d.%d.%d.x ...", "Searching Sigenergy in %d.%d.%d.x ..."), me[0], me[1], me[2]);

    if (mask[0] != 255 || mask[1] != 255 || mask[2] != 255) {
        Serial.println("[SIG] subnet larger than /24 - scanning only the /24 of this device");
    }

    for (int h = 1; h <= 254; h++) {
        if (rq_connect || rq_set_ip) return false;   // user changed something - abort
        if (h == me[3]) continue;
        IPAddress ip(me[0], me[1], me[2], h);

        LOCK();
        g_net.discover_progress = h;
        UNLOCK();

        if (probe_ip(ip, DISCOVER_TIMEOUT_MS)) {
            LOCK();
            snprintf(g_set.sig_ip, sizeof(g_set.sig_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            strlcpy(g_net.sig_ip, g_set.sig_ip, sizeof(g_net.sig_ip));
            g_net.discover_progress = 0;
            UNLOCK();
            settings_save();
            set_msg(L("Znaleziono Sigenergy: %d.%d.%d.%d", "Sigenergy found: %d.%d.%d.%d"), ip[0], ip[1], ip[2], ip[3]);
            return true;
        }
    }

    LOCK();
    g_net.discover_progress = 0;
    UNLOCK();
    set_sig_state(SIG_NOT_FOUND);
    set_msg("%s", L("Nie znaleziono. Włącz Modbus TCP w aplikacji mySigen.", "Not found. Enable Modbus TCP in the mySigen app."));
    return false;
}


// ============================================================
// PSE RCE PRICES
// ============================================================

// Parses {"period":"HH:MM - HH:MM","rce_pln":123.45} objects.
// q96 is filled with NAN first; returns the number of parsed quarters.
int pse_parse_quarters(const char *json, float *q96)
{
    for (int i = 0; i < 96; i++) q96[i] = NAN;
    int n = 0;
    const char *p = json;
    while ((p = strchr(p, '{')) != nullptr) {
        const char *end = strchr(p, '}');
        if (!end) break;

        const char *per = strstr(p, "\"period\"");
        const char *val = strstr(p, "\"rce_pln\"");
        if (per && val && per < end && val < end) {
            per = strchr(per + 8, '"');                 // opening quote of the value
            val = strchr(val + 9, ':');
            if (per && val && per < end && val < end) {
                int hh = -1, mm = -1;
                if (sscanf(per + 1, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
                    float v = strtof(val + 1, nullptr);
                    int slot = hh * 4 + mm / 15;
                    if (isnan(q96[slot])) n++;
                    q96[slot] = v;                      // DST repeated hour: last value wins
                }
            }
        }
        p = end + 1;
    }
    return n;
}

void pse_quarters_to_hours(const float *q96, float *h24)
{
    for (int h = 0; h < 24; h++) {
        float s = 0;
        int c = 0;
        for (int k = 0; k < 4; k++) {
            float v = q96[h * 4 + k];
            if (!isnan(v)) { s += v; c++; }
        }
        h24[h] = c ? s / c : NAN;
    }
}

static bool https_get(const String &url, String &body, int &code)
{
    // Not enough internal RAM for TLS -> try later instead of crashing
    if (internal_free() < HTTPS_MIN_INTERNAL || internal_largest() < HTTPS_MIN_BLOCK) {
        Serial.printf("[HTTPS] postponed - low internal RAM (free %u, largest %u)\n",
                      (unsigned)internal_free(), (unsigned)internal_largest());
        code = -100;
        return false;
    }
    sig_mem_report("before HTTPS");
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, url)) { code = -1; return false; }
    http.addHeader("Accept", "application/json");
    code = http.GET();
    if (code == 200) body = http.getString();
    http.end();
    sig_mem_report("after HTTPS");
    return code == 200;
}

// ---------- PSE RCE (PLN/MWh, Poland) ----------
static bool fetch_rce(const char *date, float *h24, float *q96, int *quarters)
{
    String url = "https://api.raporty.pse.pl/api/rce-pln?$select=period,rce_pln&$filter=business_date%20eq%20%27";
    url += date;
    url += "%27";

    String body;
    int code;
    if (!https_get(url, body, code)) {
        Serial.printf("[PSE] %s HTTP %d\n", date, code);
        return false;
    }
    int n = pse_parse_quarters(body.c_str(), q96);
    Serial.printf("[PSE] %s: %d quarters, %u bytes\n", date, n, body.length());
    if (n < 80) return false;                 // incomplete day
    pse_quarters_to_hours(q96, h24);
    if (quarters) *quarters = n;
    return true;
}

// ---------- Energy-Charts (EUR/MWh, day-ahead, many bidding zones) ----------

// Reads a JSON number array that follows "key":[ ... ]. null -> NAN.
static int json_num_array(const char *json, const char *key, double *out, int max)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p = strchr(p, '[');
    if (!p) return -1;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == ']') break;
        if (strncmp(p, "null", 4) == 0) {
            out[n++] = NAN;
            p += 4;
        } else {
            char *e;
            double v = strtod(p, &e);
            if (e == p) break;                    // unexpected token
            out[n++] = v;
            p = e;
        }
    }
    return n;
}

#define EC_MAX 500
static double *ec_ts = nullptr, *ec_val = nullptr;     // PSRAM, 8 KB

// Fills q96 (local 15-min slots of 'date') from an Energy-Charts /price answer.
// Hourly data (older days / some zones) fills all 4 quarters of the hour.
int ec_parse_day(const char *json, const char *date, float *q96)
{
    for (int i = 0; i < 96; i++) q96[i] = NAN;
    if (!ec_ts) ec_ts = (double *)ps_alloc(EC_MAX * sizeof(double));
    if (!ec_val) ec_val = (double *)ps_alloc(EC_MAX * sizeof(double));
    if (!ec_ts || !ec_val) return 0;
    int nt = json_num_array(json, "\"unix_seconds\"", ec_ts, EC_MAX);
    int nv = json_num_array(json, "\"price\"", ec_val, EC_MAX);
    int n = min(nt, nv);
    if (n <= 0) return 0;

    int filled = 0;
    for (int i = 0; i < n; i++) {
        if (isnan(ec_val[i])) continue;
        time_t t = (time_t)ec_ts[i];
        int step = (i + 1 < n) ? (int)(ec_ts[i + 1] - ec_ts[i]) : (i > 0 ? (int)(ec_ts[i] - ec_ts[i - 1]) : 900);
        if (step <= 0 || step > 3600) step = 900;
        for (int off = 0; off < step; off += 900) {
            time_t tt = t + off;
            struct tm tmv;
            localtime_r(&tt, &tmv);
            char d[11];
            strftime(d, sizeof(d), "%Y-%m-%d", &tmv);
            if (strcmp(d, date) != 0) continue;
            int slot = tmv.tm_hour * 4 + tmv.tm_min / 15;
            if (isnan(q96[slot])) filled++;
            q96[slot] = (float)ec_val[i];
        }
    }
    return filled;
}

static void url_encode_zone(const char *z, String &out)
{
    for (const char *c = z; *c; c++) {
        if (*c == '(') out += "%28";
        else if (*c == ')') out += "%29";
        else { char b[2] = {*c, 0}; out += b; }
    }
}

// One request covers today and tomorrow (the API allows 2 requests/min).
static bool fetch_ec(const Settings &st, const char *today, const char *tomorrow,
                     float *h_today, float *q_today, int *n_today,
                     float *h_tmr, float *q_tmr, int *n_tmr)
{
    time_t now = time(nullptr);
    char from[11], to[11];
    struct tm a, b;
    time_t t1 = now - 24 * 3600, t2 = now + 2 * 24 * 3600;
    localtime_r(&t1, &a);
    localtime_r(&t2, &b);
    strftime(from, sizeof(from), "%Y-%m-%d", &a);
    strftime(to, sizeof(to), "%Y-%m-%d", &b);

    String url = "https://api.energy-charts.info/price?bzn=";
    url_encode_zone(SIG_ZONES[st.zone].code, url);
    url += "&start=";
    url += from;
    url += "&end=";
    url += to;

    String body;
    int code;
    if (!https_get(url, body, code)) {
        Serial.printf("[EC] %s HTTP %d%s\n", SIG_ZONES[st.zone].code, code, code == 429 ? " (rate limit)" : "");
        return false;
    }

    // EUR/MWh -> display currency/MWh
    float fx = (st.currency == CUR_EUR) ? 1.0f : st.fx_rate;

    *n_today = ec_parse_day(body.c_str(), today, q_today);
    *n_tmr = ec_parse_day(body.c_str(), tomorrow, q_tmr);
    for (int i = 0; i < 96; i++) {
        if (!isnan(q_today[i])) q_today[i] *= fx;
        if (!isnan(q_tmr[i])) q_tmr[i] *= fx;
    }
    pse_quarters_to_hours(q_today, h_today);
    pse_quarters_to_hours(q_tmr, h_tmr);
    Serial.printf("[EC] %s: today %d, tomorrow %d quarters (%u bytes)\n",
                  SIG_ZONES[st.zone].code, *n_today, *n_tmr, body.length());
    return *n_today >= 80;
}

static void date_str(time_t t, char *buf)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, 11, "%Y-%m-%d", &tmv);
}

static uint32_t last_price_try = 0;
static uint32_t price_key = 0xFFFFFFFF;

static void prices_reset()
{
    LOCK();
    g_prices.ok_today = g_prices.ok_tomorrow = false;
    g_prices.quarters_today = 0;
    g_prices.date_today[0] = 0;
    for (int i = 0; i < 24; i++) { g_prices.today[i] = NAN; g_prices.tomorrow[i] = NAN; }
    for (int i = 0; i < 96; i++) { g_prices.q_today[i] = NAN; g_prices.q_tomorrow[i] = NAN; }
    UNLOCK();
    last_price_try = 0;
}

static void prices_task_step()
{
    time_t now = time(nullptr);
    if (now < 1700000000) return;                 // no valid time yet

    Settings st;
    LOCK();
    st = g_set;
    UNLOCK();
    if (st.price_src == SRC_FIXED) return;        // fixed price: nothing to download

    // source / zone / currency changed -> start over
    uint32_t key = (st.price_src << 24) ^ (st.zone << 16) ^ (st.currency << 12) ^ (uint32_t)(st.fx_rate * 1000);
    if (key != price_key) {
        price_key = key;
        prices_reset();
    }

    char today[11], tomorrow[11];
    date_str(now, today);
    date_str(now + 24 * 3600, tomorrow);

    struct tm tmv;
    localtime_r(&now, &tmv);

    PriceData p;
    LOCK();
    p = g_prices;
    UNLOCK();

    bool new_day = strcmp(p.date_today, today) != 0;
    if (new_day && p.ok_tomorrow && p.date_today[0]) {
        // yesterday's "tomorrow" becomes today
        memcpy(p.today, p.tomorrow, sizeof(p.today));
        memcpy(p.q_today, p.q_tomorrow, sizeof(p.q_today));
        p.ok_today = true;
        p.ok_tomorrow = false;
        strlcpy(p.date_today, today, sizeof(p.date_today));
        LOCK();
        g_prices = p;
        UNLOCK();
        new_day = false;
    }

    bool need_today = new_day || !p.ok_today || (millis() - p.fetched_ms > PRICE_REFRESH_MS && p.quarters_today < 92);
    // day-ahead results: EPEX ~12:45 CET, PSE RCE ~14:00
    bool need_tomorrow = !p.ok_tomorrow && tmv.tm_hour >= (st.price_src == SRC_EC ? 13 : 14);

    if (!need_today && !need_tomorrow) return;
    if (last_price_try && millis() - last_price_try < PRICE_RETRY_MS && !new_day) return;
    last_price_try = millis();

    static float h1[24], h2[24], q1[96], q2[96];

    if (st.price_src == SRC_EC) {
        int n1 = 0, n2 = 0;
        if (fetch_ec(st, today, tomorrow, h1, q1, &n1, h2, q2, &n2)) {
            LOCK();
            memcpy(g_prices.today, h1, sizeof(h1));
            memcpy(g_prices.q_today, q1, sizeof(q1));
            g_prices.ok_today = true;
            g_prices.quarters_today = n1;
            strlcpy(g_prices.date_today, today, sizeof(g_prices.date_today));
            g_prices.fetched_ms = millis();
            if (n2 >= 80) {
                memcpy(g_prices.tomorrow, h2, sizeof(h2));
                memcpy(g_prices.q_tomorrow, q2, sizeof(q2));
                g_prices.ok_tomorrow = true;
            } else {
                g_prices.ok_tomorrow = false;
            }
            UNLOCK();
        }
        return;
    }

    // ---- PSE RCE ----
    if (need_today) {
        int q = 0;
        if (fetch_rce(today, h1, q1, &q)) {
            LOCK();
            memcpy(g_prices.today, h1, sizeof(h1));
            memcpy(g_prices.q_today, q1, sizeof(q1));
            g_prices.ok_today = true;
            g_prices.quarters_today = q;
            strlcpy(g_prices.date_today, today, sizeof(g_prices.date_today));
            g_prices.fetched_ms = millis();
            if (new_day) g_prices.ok_tomorrow = false;
            UNLOCK();
        }
    }
    if (need_tomorrow) {
        if (fetch_rce(tomorrow, h2, q2, nullptr)) {
            LOCK();
            memcpy(g_prices.tomorrow, h2, sizeof(h2));
            memcpy(g_prices.q_tomorrow, q2, sizeof(q2));
            g_prices.ok_tomorrow = true;
            UNLOCK();
        }
    }
}


// ============================================================
// MONEY / ENERGY ACCOUNTING
// ============================================================

// ---------- SOC history (for the price chart) ----------
static void soc_hist_clear()
{
    memset(g_soc_hist, 255, sizeof(g_soc_hist));
}

static void soc_record(const SigData &d)
{
    time_t now = time(nullptr);
    if (now < 1700000000) return;
    struct tm t;
    localtime_r(&now, &t);
    int day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    int slot = t.tm_hour * 4 + t.tm_min / 15;
    LOCK();
    if (day != g_soc_day) {
        soc_hist_clear();
        g_soc_day = day;
    }
    float soc = d.soc < 0 ? 0 : (d.soc > 100 ? 100 : d.soc);
    g_soc_hist[slot] = (uint8_t)(soc + 0.5f);
    UNLOCK();
}

static double prev_pv, prev_load, prev_imp, prev_exp, prev_bc, prev_bd;
static bool   prev_ok = false;
static uint32_t prev_ms = 0;

static void money_add(Money &m, double pv, double load, double imp, double exp_,
                      double bc, double bd, double saved, double expv, double impc,
                      double dep_out)
{
    m.kwh_pv += pv;   m.kwh_load += load;
    m.kwh_imp += imp; m.kwh_exp += exp_;
    m.kwh_bchg += bc; m.kwh_bdis += bd;
    m.saved_pln += saved;
    m.export_pln += expv;
    m.import_cost_pln += impc;
    m.dep_in_pln += expv;
    m.dep_out_pln += dep_out;
}

static double earn(const Money &m) { return m.saved_pln + m.export_pln; }

// caller holds the lock
static void build_dayrec(DayRec &r, uint32_t date)
{
    memset(&r, 0, sizeof(r));
    const Money &t = g_money.today;
    r.date = date;
    r.kwh_pv = t.kwh_pv;     r.kwh_load = t.kwh_load;
    r.kwh_imp = t.kwh_imp;   r.kwh_exp = t.kwh_exp;
    r.kwh_bchg = t.kwh_bchg; r.kwh_bdis = t.kwh_bdis;
    r.saved = t.saved_pln;   r.exp_val = t.export_pln;  r.imp_cost = t.import_cost_pln;
    r.dep_in = t.dep_in_pln; r.dep_out = t.dep_out_pln; r.deposit_end = g_money.deposit;
    r.soc_min = g_dstat.soc_min > 100 ? 0 : g_dstat.soc_min;
    r.soc_max = g_dstat.soc_max;
    r.pv_peak_kw = g_dstat.pv_peak_kw;
    r.imp_peak_kw = g_dstat.imp_peak_kw;
    r.price_avg = g_dstat.price_hours > 0 ? g_dstat.price_sum / g_dstat.price_hours : NAN;
    r.currency = g_set.currency;
    r.price_src = g_set.price_src;
    r.zone = g_set.zone;
}

// finished quarter -> storage
static bool money_save_due = false;

static void quarter_flush(const QAcc &q)
{
    auto wh = [](double kwh) -> uint16_t {
        double w = kwh * 1000.0 + 0.5;
        return (uint16_t)(w < 0 ? 0 : (w > 65535 ? 65535 : w));
    };
    QRec r;
    memset(&r, 0, sizeof(r));
    r.ts = q.ts;
    r.price_market = q.pok ? q.pm : NAN;
    r.price_export = q.pok ? q.pe : NAN;
    r.pv_wh = wh(q.pv);     r.load_wh = wh(q.load);
    r.imp_wh = wh(q.imp);   r.exp_wh = wh(q.exp_);
    r.bchg_wh = wh(q.bc);   r.bdis_wh = wh(q.bd);
    r.soc = q.soc;
    r.flags = (q.pok ? 1 : 0) | (g_set.currency << 4);
    r.saved = q.saved;  r.exp_val = q.expv;  r.imp_cost = q.impc;  r.dep_out = q.dep_out;
    store_append_quarter(r);
    money_save_due = true;          // counters are saved right after the quarter record (one glitch window)
}

static void check_rollover()
{
    time_t now = time(nullptr);
    if (now < 1700000000) return;
    struct tm t;
    localtime_r(&now, &t);
    int day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    int mon = (t.tm_year + 1900) * 100 + (t.tm_mon + 1);

    bool changed = false;
    bool write_day = false;
    DayRec rec;
    LOCK();
    if (money_day == 0) money_day = day;
    if (money_mon == 0) money_mon = mon;
    if (day != money_day) {
        build_dayrec(rec, money_day);
        write_day = true;
        memset(&g_dstat, 0, sizeof(g_dstat));
        g_dstat.soc_min = 255;
        g_money.yesterday_earn = earn(g_money.today);
        g_money.yesterday_dep = g_money.today.dep_in_pln - g_money.today.dep_out_pln;
        memset(&g_money.today, 0, sizeof(Money));
        money_day = day;
        changed = true;
    }
    if (mon != money_mon) {
        memset(&g_money.month, 0, sizeof(Money));
        money_mon = mon;
        changed = true;
    }
    UNLOCK();
    if (write_day) store_append_day(rec);
    if (changed) money_save(true);
}

// Market price for "now" in the selected resolution [per kWh], before coefficient.
static float market_price_now(const Settings &st, bool &ok)
{
    ok = false;
    if (st.price_src == SRC_FIXED) {
        ok = true;
        return st.fixed_export_price;
    }
    time_t now = time(nullptr);
    if (now < 1700000000) return 0;
    struct tm t;
    localtime_r(&now, &t);
    char today[11];
    date_str(now, today);

    float v = NAN;
    LOCK();
    if (g_prices.ok_today && strcmp(g_prices.date_today, today) == 0) {
        if (st.price_res == RES_QUARTER) {
            v = g_prices.q_today[t.tm_hour * 4 + t.tm_min / 15];
            if (isnan(v)) v = g_prices.today[t.tm_hour];
        } else {
            v = g_prices.today[t.tm_hour];
        }
    }
    UNLOCK();
    if (isnan(v)) return 0;
    ok = true;
    return v / 1000.0f;          // currency/MWh -> currency/kWh
}

// Value of 1 exported kWh
static float export_value(const Settings &st, float market, bool ok)
{
    if (!ok) return 0;
    float v = market;
    if (st.neg_as_zero && v < 0) v = 0;
    if (st.price_src == SRC_RCE) v *= st.export_coef;     // 1.23 is Polish net-billing (RCE) only
    return v;
}

static double counter_delta(bool have, double cur, double prev, double fallback)
{
    if (!have) return fallback;
    double d = cur - prev;
    if (d < 0 || d > 5.0) return fallback;     // counter reset / glitch
    return d;
}

static void account(const SigData &d)
{
    uint32_t now = millis();
    if (!prev_ok) {
        prev_pv = d.acc_pv; prev_load = d.acc_load;
        prev_imp = d.acc_imp; prev_exp = d.acc_exp;
        prev_bc = d.acc_bchg; prev_bd = d.acc_bdis;
        prev_ms = now;
        prev_ok = true;
        return;
    }

    double dt_h = (now - prev_ms) / 3600000.0;
    prev_ms = now;
    if (dt_h <= 0 || dt_h > 60.0 / 3600.0) dt_h = 0;     // gap > 60 s: don't integrate power

    // power-based fallbacks
    double f_imp  = (d.grid_kw > 0 ? d.grid_kw : 0) * dt_h;
    double f_exp  = (d.grid_kw < 0 ? -d.grid_kw : 0) * dt_h;
    double f_load = (d.load_kw > 0 ? d.load_kw : 0) * dt_h;
    double f_pv   = (d.pv_kw > 0 ? d.pv_kw : 0) * dt_h;
    double f_bc   = (d.ess_kw > 0 ? d.ess_kw : 0) * dt_h;
    double f_bd   = (d.ess_kw < 0 ? -d.ess_kw : 0) * dt_h;

    double imp  = counter_delta(d.have_acc_grid, d.acc_imp,  prev_imp,  f_imp);
    double exp_ = counter_delta(d.have_acc_grid, d.acc_exp,  prev_exp,  f_exp);
    double load = counter_delta(d.have_acc_load, d.acc_load, prev_load, f_load);
    double pv   = counter_delta(d.have_acc_pv,   d.acc_pv,   prev_pv,   f_pv);
    double bc   = counter_delta(d.have_acc_batt, d.acc_bchg, prev_bc,   f_bc);
    double bd   = counter_delta(d.have_acc_batt, d.acc_bdis, prev_bd,   f_bd);

    prev_pv = d.acc_pv; prev_load = d.acc_load;
    prev_imp = d.acc_imp; prev_exp = d.acc_exp;
    prev_bc = d.acc_bchg; prev_bd = d.acc_bdis;

    Settings s;
    LOCK();
    s = g_set;
    UNLOCK();

    bool pok;
    float market = market_price_now(s, pok);
    float exp_price = export_value(s, market, pok);

    double self = load - imp;                            // consumption not bought from grid
    if (self < 0) self = 0;

    double saved = self * s.import_price;
    double expv  = exp_ * exp_price;
    double impc  = imp * s.import_price;
    double dep_out = imp * s.energy_price;               // energy part of the bill is paid from the deposit

    // instantaneous earning rate [PLN/h]
    float self_kw = d.load_kw - (d.grid_kw > 0 ? d.grid_kw : 0);
    if (self_kw < 0) self_kw = 0;
    float exp_kw = d.grid_kw < 0 ? -d.grid_kw : 0;
    float imp_kw = d.grid_kw > 0 ? d.grid_kw : 0;
    float rate = self_kw * s.import_price + exp_kw * exp_price;
    float rate_dep = exp_kw * exp_price - imp_kw * s.energy_price;

    LOCK();
    money_add(g_money.today, pv, load, imp, exp_, bc, bd, saved, expv, impc, dep_out);
    money_add(g_money.month, pv, load, imp, exp_, bc, bd, saved, expv, impc, dep_out);
    money_add(g_money.total, pv, load, imp, exp_, bc, bd, saved, expv, impc, dep_out);
    g_money.deposit += expv - dep_out;
    if (g_money.deposit < 0) g_money.deposit = 0;        // the rest is paid in cash, deposit never < 0
    g_money.rate_pln_h = rate;
    g_money.rate_dep_h = rate_dep;
    g_money.price_now_pln_kwh = market;
    g_money.export_price_now = exp_price;
    g_money.price_now_ok = pok;

    // day statistics
    uint8_t soc8 = (uint8_t)(d.soc < 0 ? 0 : (d.soc > 100 ? 100 : d.soc + 0.5f));
    if (g_dstat.soc_min > 100 || soc8 < g_dstat.soc_min) g_dstat.soc_min = soc8;
    if (soc8 > g_dstat.soc_max) g_dstat.soc_max = soc8;
    if (d.pv_kw > g_dstat.pv_peak_kw) g_dstat.pv_peak_kw = d.pv_kw;
    if (d.grid_kw > g_dstat.imp_peak_kw) g_dstat.imp_peak_kw = d.grid_kw;
    if (pok && dt_h > 0) { g_dstat.price_sum += market * dt_h; g_dstat.price_hours += dt_h; }
    UNLOCK();

    // 15-minute record
    time_t tnow = time(nullptr);
    if (tnow > 1700000000) {
        uint32_t qts = (uint32_t)(tnow / 900) * 900;
        if (g_qacc.any && g_qacc.ts != qts) {
            quarter_flush(g_qacc);
            memset(&g_qacc, 0, sizeof(g_qacc));
        }
        if (!g_qacc.any) { g_qacc.any = true; g_qacc.ts = qts; }
        g_qacc.pv += pv;  g_qacc.load += load;  g_qacc.imp += imp;  g_qacc.exp_ += exp_;
        g_qacc.bc += bc;  g_qacc.bd += bd;
        g_qacc.saved += saved;  g_qacc.expv += expv;  g_qacc.impc += impc;  g_qacc.dep_out += dep_out;
        g_qacc.pm = market;  g_qacc.pe = exp_price;  g_qacc.pok = pok;  g_qacc.soc = soc8;
    }
}


// ============================================================
// WIFI
// ============================================================

static uint32_t wifi_started_ms = 0;
static bool wifi_attempting = false;
static bool time_configured = false;
static char active_tz[48] = "";

// Country change -> device time zone follows (day boundaries, clock, prices)
static void apply_tz_if_changed()
{
    Settings s;
    LOCK();
    s = g_set;
    UNLOCK();
    const char *tz = sig_zone_tz(s);
    if (strcmp(tz, active_tz) == 0) return;
    strlcpy(active_tz, tz, sizeof(active_tz));
    setenv("TZ", active_tz, 1);
    tzset();
    Serial.printf("[SIG] time zone: %s\n", active_tz);
}

static void wifi_begin_connect()
{
    Settings s;
    LOCK();
    s = g_set;
    g_net.wifi_connecting = s.ssid[0] != 0;
    UNLOCK();
    if (!s.ssid[0]) return;

    Serial.printf("[WIFI] connecting to %s\n", s.ssid);
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(s.ssid, s.pass);
    wifi_started_ms = millis();
    wifi_attempting = true;
}

static void wifi_do_scan()
{
    LOCK();
    g_net.scanning = true;
    UNLOCK();

    if (wifi_attempting) { WiFi.disconnect(); wifi_attempting = false; }
    int n = WiFi.scanNetworks(false, false);
    Serial.printf("[WIFI] scan: %d\n", n);

    LOCK();
    g_net.scan_count = 0;
    for (int i = 0; i < n && g_net.scan_count < SIG_WIFI_SCAN_MAX; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        bool dup = false;
        for (int j = 0; j < g_net.scan_count; j++)
            if (strcmp(g_net.scan_ssid[j], s.c_str()) == 0) dup = true;
        if (dup) continue;
        strlcpy(g_net.scan_ssid[g_net.scan_count], s.c_str(), 33);
        g_net.scan_rssi[g_net.scan_count] = WiFi.RSSI(i);
        g_net.scan_count++;
    }
    g_net.scanning = false;
    g_net.scan_seq++;
    UNLOCK();
    WiFi.scanDelete();
}

static void update_wifi_status()
{
    bool ok = WiFi.status() == WL_CONNECTED;
    LOCK();
    g_net.wifi_ok = ok;
    if (ok) {
        strlcpy(g_net.ssid, WiFi.SSID().c_str(), sizeof(g_net.ssid));
        g_net.rssi = WiFi.RSSI();
        strlcpy(g_net.ip, WiFi.localIP().toString().c_str(), sizeof(g_net.ip));
        g_net.wifi_connecting = false;
    }
    g_net.time_ok = time(nullptr) > 1700000000;
    UNLOCK();
}


// ============================================================
// NETWORK TASK
// ============================================================

static uint32_t discover_retry_at = 0;   // millis() of the next Sig try after a failed one (0 = now)
static uint32_t next_full_scan = 0;      // millis() of the next full network scan (0 = now)
static uint8_t  scans_failed = 0;

// current market / export price for the UI - works without Sigenergy
static void price_now_update()
{
    static uint32_t last = 0;
    if (last && millis() - last < 1000) return;
    last = millis();

    Settings s;
    LOCK();
    s = g_set;
    UNLOCK();
    bool pok;
    float market = market_price_now(s, pok);
    float exp_price = export_value(s, market, pok);
    LOCK();
    g_money.price_now_pln_kwh = market;
    g_money.export_price_now = exp_price;
    g_money.price_now_ok = pok;
    UNLOCK();
}

static void net_task(void *)
{
    WiFi.persistent(false);         // credentials live in our own settings - no NVS writes on (re)connect
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_begin_connect();

    uint32_t last_poll = 0;
    uint32_t last_save = millis();
    uint32_t last_wifi_retry = 0;

    for (;;) {
        // ---------- requests from UI ----------
        if (rq_save_settings) {
            rq_save_settings = false;
            settings_save();
            if (time_configured) apply_tz_if_changed();
        }
        if (rq_save_money) {
            rq_save_money = false;
            money_save();
        }
        if (rq_scan) {
            rq_scan = false;
            discover_retry_at = 0;
            wifi_do_scan();
            if (!(WiFi.status() == WL_CONNECTED)) wifi_begin_connect();
        }
        if (rq_connect) {
            rq_connect = false;
            LOCK();
            strlcpy(g_set.ssid, rq_ssid, sizeof(g_set.ssid));
            strlcpy(g_set.pass, rq_pass, sizeof(g_set.pass));
            UNLOCK();
            settings_save();
            mb.stop();
            discover_retry_at = 0;
            next_full_scan = 0;
            scans_failed = 0;
            wifi_begin_connect();
        }
        if (rq_set_ip) {
            rq_set_ip = false;
            LOCK();
            strlcpy(g_set.sig_ip, rq_ip, sizeof(g_set.sig_ip));
            strlcpy(g_net.sig_ip, rq_ip, sizeof(g_net.sig_ip));
            UNLOCK();
            settings_save();
            mb.stop();
            consecutive_fail = 0;
            for (int b = 0; b < B_COUNT; b++) blocks[b].supported = true;
        }
        if (rq_reset_money) {
            rq_reset_money = false;
            LOCK();
            memset(&g_money.today, 0, sizeof(Money));
            memset(&g_money.month, 0, sizeof(Money));
            memset(&g_money.total, 0, sizeof(Money));
            g_money.yesterday_earn = 0;
            g_money.deposit = 0;
            UNLOCK();
            money_save(true);
        }

        // ---------- WiFi ----------
        update_wifi_status();
        bool wifi_ok = WiFi.status() == WL_CONNECTED;

        if (!wifi_ok) {
            set_sig_state(SIG_NO_WIFI);
            if (wifi_attempting && millis() - wifi_started_ms > 20000) {
                wifi_attempting = false;
                LOCK();
                g_net.wifi_connecting = false;
                UNLOCK();
                set_msg("%s", L("Brak połączenia z Wi-Fi", "No Wi-Fi connection"));
            }
            if (!wifi_attempting && millis() - last_wifi_retry > 30000) {
                last_wifi_retry = millis();
                wifi_begin_connect();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        wifi_attempting = false;
        static bool logged_conn = false;
        if (!logged_conn) {
            logged_conn = true;
            Serial.printf("[WIFI] connected, IP %s, RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
            sig_mem_report("wifi connected");
        }

        if (!time_configured) {
            LOCK();
            Settings ts = g_set;
            UNLOCK();
            strlcpy(active_tz, sig_zone_tz(ts), sizeof(active_tz));
            configTzTime(active_tz, "pool.ntp.org", "time.google.com");
            time_configured = true;
        }

        // ---------- Sigenergy ----------
        Settings s;
        LOCK();
        s = g_set;
        UNLOCK();

        // Sig not found: the saved IP is re-tried every 60 s (one TCP connect),
        // a full network scan (254 connects) only with a growing pause:
        // 2, 5, 10, 15 min... - constant scanning loads WiFi/PSRAM and disturbs the display.
        bool sig_ready = true;
        if (rq_discover || !s.sig_ip[0] || consecutive_fail >= 6) {
            bool manual = rq_discover;
            if (!manual && discover_retry_at && (int32_t)(millis() - discover_retry_at) < 0) {
                sig_ready = false;
            } else {
                rq_discover = false;
                mb.stop();
                consecutive_fail = 0;

                // first try the saved address
                IPAddress saved;
                bool found = false;
                if (s.sig_ip[0] && saved.fromString(s.sig_ip)) {
                    set_sig_state(SIG_CONNECTING);
                    found = probe_ip(saved, 1500);
                }
                if (!found && (manual || !next_full_scan || (int32_t)(millis() - next_full_scan) >= 0)) {
                    found = discover();
                    if (!found) {
                        static const uint16_t pause_min[] = {2, 5, 10, 15};
                        uint32_t m = pause_min[scans_failed < 3 ? scans_failed : 3];
                        if (scans_failed < 250) scans_failed++;
                        next_full_scan = millis() + m * 60000UL;
                        if (!next_full_scan) next_full_scan = 1;
                        Serial.printf("[SIG] not found, next network scan in %u min\n", (unsigned)m);
                    }
                }
                if (!found) {
                    discover_retry_at = millis() + 60000;
                    if (!discover_retry_at) discover_retry_at = 1;
                    consecutive_fail = 6;           // keep the saved IP, but don't poll it until the next try
                    sig_ready = false;
                    set_sig_state(SIG_NOT_FOUND);
                    set_msg("%s", L("Nie znaleziono. Włącz Modbus TCP w aplikacji mySigen.", "Not found. Enable Modbus TCP in the mySigen app."));
                } else {
                    discover_retry_at = 0;
                    next_full_scan = 0;
                    scans_failed = 0;
                    LOCK();
                    s = g_set;
                    UNLOCK();
                }
            }
        }

        if (sig_ready && millis() - last_poll >= POLL_PERIOD_MS) {
            last_poll = millis();

            IPAddress ip;
            ip.fromString(s.sig_ip);
            if (!mb.connected()) set_sig_state(SIG_CONNECTING);

            bool ok = mb.connect(ip) && poll_once(s.inv_id);
            if (ok) {
                consecutive_fail = 0;
                set_sig_state(SIG_OK);
                LOCK();
                strlcpy(g_net.sig_ip, s.sig_ip, sizeof(g_net.sig_ip));
                SigData d = g_data;
                UNLOCK();

                // no NTP yet? use the time from the Sigenergy system
                if (time(nullptr) < 1700000000 && d.sys_time > 1700000000) {
                    struct timeval tv = {(time_t)d.sys_time, 0};
                    settimeofday(&tv, nullptr);
                    setenv("TZ", active_tz[0] ? active_tz : TZ_POLAND, 1);
                    tzset();
                }

                check_rollover();
                account(d);
                soc_record(d);
            } else {
                consecutive_fail++;
                mb.stop();
                set_sig_state(SIG_ERROR);
                set_msg(L("Brak odpowiedzi z %s (%d)", "No response from %s (%d)"), s.sig_ip, consecutive_fail);
            }
        }

        // ---------- prices (independent of Sigenergy) ----------
        prices_task_step();
        price_now_update();

        // ---------- storage info for the UI + memory log ----------
        static uint32_t last_info = 0;
        if (millis() - last_info > 60000) {
            last_info = millis();
            sig_mem_report("periodic");     // (no file-system access here: it stalls the display)
        }

        // ---------- periodic save ----------
        if (money_save_due || millis() - last_save > SAVE_PERIOD_MS) {
            money_save_due = false;
            last_save = millis();
            money_save();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


// ============================================================
// PUBLIC API
// ============================================================

static const char *reset_reason_text()
{
#ifdef ESP_PLATFORM
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "PANIC / exception";
    case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT:      return "OTHER WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT - power supply";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "other";
    }
#else
    return "host";
#endif
}

void sig_backend_begin()
{
    Serial.printf("[BOOT] reset reason: %s\n", reset_reason_text());
    sig_mem_report("boot");
    g_mux = xSemaphoreCreateMutex();
    memset(&g_data, 0, sizeof(g_data));
    memset(&g_prices, 0, sizeof(g_prices));
    memset(&g_money, 0, sizeof(g_money));
    memset(&g_net, 0, sizeof(g_net));
    memset(&g_set, 0, sizeof(g_set));
    for (int i = 0; i < 24; i++) { g_prices.today[i] = NAN; g_prices.tomorrow[i] = NAN; }
    for (int i = 0; i < 96; i++) { g_prices.q_today[i] = NAN; g_prices.q_tomorrow[i] = NAN; }

    soc_hist_clear();
    memset(&g_dstat, 0, sizeof(g_dstat));
    g_dstat.soc_min = 255;
    memset(&g_qacc, 0, sizeof(g_qacc));
    settings_load();
    money_load();
    strlcpy(g_net.sig_ip, g_set.sig_ip, sizeof(g_net.sig_ip));
    Serial.printf("[SIG] settings: ssid='%s' sig=%s inv=%d price=%.2f coef=%.2f\n",
                  g_set.ssid, g_set.sig_ip, g_set.inv_id, g_set.import_price, g_set.export_coef);

    // 16 KB stack: HTTPS (TLS) needs it. Core 0 = next to the WiFi stack, away from LVGL.
    xTaskCreatePinnedToCore(net_task, "sig_net", 16384, nullptr, 1, nullptr, 0);
}

void sig_get_data(SigData &out)      { LOCK(); out = g_data;   UNLOCK(); }
void sig_get_prices(PriceData &out)  { LOCK(); out = g_prices; UNLOCK(); }
void sig_get_money(MoneyData &out)   { LOCK(); out = g_money;  UNLOCK(); }
void sig_get_status(NetStatus &out)  { LOCK(); out = g_net;    UNLOCK(); }
void sig_get_settings(Settings &out) { LOCK(); out = g_set;    UNLOCK(); }

void sig_get_today_dayrec(DayRec &out)
{
    LOCK();
    build_dayrec(out, date_yyyymmdd(time(nullptr)));
    UNLOCK();
}

void sig_get_soc_history(uint8_t out[96])
{
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    int day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    LOCK();
    if (day == g_soc_day) memcpy(out, g_soc_hist, 96);
    else memset(out, 255, 96);                 // saved history is from another day
    UNLOCK();
}

void sig_req_wifi_scan() { rq_scan = true; }

void sig_req_wifi_connect(const char *ssid, const char *pass)
{
    LOCK();
    strlcpy(rq_ssid, ssid, sizeof(rq_ssid));
    strlcpy(rq_pass, pass, sizeof(rq_pass));
    UNLOCK();
    rq_connect = true;
}

void sig_req_discover() { rq_discover = true; }

void sig_req_set_ip(const char *ip)
{
    LOCK();
    strlcpy(rq_ip, ip, sizeof(rq_ip));
    UNLOCK();
    rq_set_ip = true;
}

void sig_req_set_inv_id(uint8_t id)
{
    LOCK();
    g_set.inv_id = id;
    UNLOCK();
    rq_save_settings = true;
}

void sig_req_set_price(float p)
{
    LOCK();
    g_set.import_price = p;
    UNLOCK();
    rq_save_settings = true;
}

void sig_req_set_coef(float c)
{
    LOCK();
    g_set.export_coef = c;
    UNLOCK();
    rq_save_settings = true;
}

void sig_req_set_neg_zero(bool on)
{
    LOCK();
    g_set.neg_as_zero = on;
    UNLOCK();
    rq_save_settings = true;
}

void sig_req_reset_money() { rq_reset_money = true; }

void sig_lcd_defaults()
{
    LOCK();
    g_set.lcd_clk = SIG_LCD_CLK_DEFAULT;
    g_set.lcd_bb = 1;
    UNLOCK();
    Preferences p;
    p.begin("sigdash", false);
    p.putUChar("lcdmhz", 16);
    p.putUChar("lcdbb", 1);
    p.putUChar("lcdbb_try", 0);
    p.end();
}

void sig_req_update_prefs(const Settings &n)
{
    LOCK();
    g_set.inv_id = n.inv_id;
    g_set.import_price = n.import_price;
    g_set.export_coef = n.export_coef;
    g_set.neg_as_zero = n.neg_as_zero;
    g_set.lang = n.lang;
    g_set.earn_mode = n.earn_mode;
    g_set.price_src = n.price_src;
    g_set.price_res = n.price_res;
    g_set.zone = n.zone < SIG_ZONE_COUNT ? n.zone : SIG_ZONE_PL;
    g_set.fx_rate = n.fx_rate;
    g_set.fixed_export_price = n.fixed_export_price;
    g_set.energy_price = n.energy_price;
    g_set.currency = n.currency;
    g_set.lcd_clk = n.lcd_clk <= 2 ? n.lcd_clk : SIG_LCD_CLK_DEFAULT;
    g_set.lcd_bb = n.lcd_bb <= 2 ? n.lcd_bb : 1;
    UNLOCK();
    rq_save_settings = true;
}

void sig_req_set_deposit(double v)
{
    LOCK();
    g_money.deposit = v < 0 ? 0 : v;
    UNLOCK();
    rq_save_money = true;
}
