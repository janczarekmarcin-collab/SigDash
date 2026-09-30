#pragma once
// ============================================================
// SigDash - backend: WiFi, Sigenergy Modbus TCP, PSE RCE prices,
// energy / money accounting.
//
// All network work runs in its own FreeRTOS task (core 0).
// The UI (LVGL task) only reads snapshots via sig_get_*().
// Nothing here touches LVGL.
// ============================================================

#include <Arduino.h>

#define SIG_WIFI_SCAN_MAX 12

// ---------------- live data from Sigenergy ----------------
struct SigData {
    bool     valid;            // at least one successful plant read
    uint32_t updated_ms;       // millis() of last successful poll
    uint32_t poll_count;

    // plant (unit 247)
    uint32_t sys_time;         // epoch seconds from the system
    int16_t  tz_min;
    uint16_t ems_mode;
    uint16_t grid_sensor_ok;
    uint16_t on_off_grid;
    uint16_t run_state;
    float    grid_kw;          // >0 import, <0 export
    float    grid_ph_kw[3];
    float    pv_kw;
    float    ess_kw;           // >0 charging, <0 discharging
    float    plant_kw;
    float    load_kw;
    bool     load_from_reg;    // true: register 30284, false: computed
    float    soc;
    float    soh;
    float    ess_rated_kwh;
    float    ess_avail_chg_kwh;
    float    ess_avail_dis_kwh;
    float    ess_max_chg_kw;
    float    ess_max_dis_kw;
    float    chg_cutoff_soc;
    float    dis_cutoff_soc;
    uint16_t alarms[7];

    // counters [kWh] (plant)
    double   acc_pv, acc_load, acc_imp, acc_exp, acc_bchg, acc_bdis;
    bool     have_acc_pv, have_acc_load, have_acc_grid, have_acc_batt;
    float    pv_today_reg, pv_yday_reg;
    bool     have_pv_daily;
    float    plant_cell_temp;

    // inverter (unit = inverter id)
    bool     inv_ok;
    uint16_t inv_state;
    float    inv_kw;
    float    inv_temp;
    float    grid_freq;
    float    v_ph[3];
    float    i_ph[3];
    float    pf;
    int      mppt_count;
    float    pv_v[4], pv_i[4];
    float    inv_pv_kw;
    float    insulation_mohm;
    float    bat_temp_avg, bat_temp_max, bat_temp_min;
    float    cell_v_avg, cell_v_max, cell_v_min;
    float    ess_day_chg_kwh, ess_day_dis_kwh;
    uint16_t inv_alarms[5];
};

// ---------------- prices ----------------
struct PriceData {
    // all prices are in the display currency per MWh
    bool    ok_today;
    bool    ok_tomorrow;
    float   today[24];         // PLN/MWh, hourly average of 15-min RCE
    float   tomorrow[24];
    float   q_today[96];       // PLN/MWh, raw 15-min RCE
    float   q_tomorrow[96];
    int     quarters_today;
    char    date_today[11];
    uint32_t fetched_ms;
};

// ---------------- money ----------------
struct Money {
    double saved_pln;          // self-consumption x import price
    double export_pln;         // export x RCE x coefficient
    double import_cost_pln;    // grid import x import price
    double kwh_pv, kwh_load, kwh_imp, kwh_exp, kwh_bchg, kwh_bdis;
    double dep_in_pln;         // prosumer account: value of exported energy
    double dep_out_pln;        // prosumer account: energy part of imports paid from it
};

struct MoneyData {
    Money   today, month, total;
    double  yesterday_earn;
    double  yesterday_dep;     // net change of the prosumer account yesterday
    float   rate_pln_h;        // current earning speed (savings + export)
    float   rate_dep_h;        // current prosumer-account change speed
    float   price_now_pln_kwh; // market price now [per kWh] (before coefficient)
    float   export_price_now;  // what 1 exported kWh is worth now
    bool    price_now_ok;
    double  deposit;           // prosumer account balance
};

// ---------------- network / status ----------------
enum SigState : uint8_t {
    SIG_IDLE = 0,
    SIG_NO_WIFI,
    SIG_SEARCHING,
    SIG_CONNECTING,
    SIG_OK,
    SIG_ERROR,
    SIG_NOT_FOUND
};

struct NetStatus {
    bool     wifi_ok;
    char     ssid[33];
    int      rssi;
    char     ip[16];
    bool     wifi_connecting;
    bool     time_ok;

    SigState sig_state;
    char     sig_ip[16];
    int      discover_progress;   // 0..254
    char     msg[64];

    // WiFi scan
    bool     scanning;
    uint32_t scan_seq;
    int      scan_count;
    char     scan_ssid[SIG_WIFI_SCAN_MAX][33];
    int      scan_rssi[SIG_WIFI_SCAN_MAX];
};

// ---------------- settings ----------------
enum : uint8_t { LANG_PL = 0, LANG_EN = 1 };
enum : uint8_t { EARN_TOTAL = 0, EARN_DEPOSIT = 1 };
enum : uint8_t { SRC_RCE = 0, SRC_EC = 1, SRC_FIXED = 2 };   // PSE RCE / Energy-Charts / fixed
enum : uint8_t { RES_HOUR = 0, RES_QUARTER = 1 };

// Bidding zones available from api.energy-charts.info (day-ahead, EUR/MWh)
struct PriceZone {
    const char *code;     // bzn parameter
    const char *pl;       // country name (PL)
    const char *en;       // country name (EN)
    const char *tz;       // POSIX time zone of the zone
};
extern const PriceZone SIG_ZONES[];
extern const int SIG_ZONE_COUNT;
#define SIG_ZONE_PL 0     // index of Poland in SIG_ZONES
enum : uint8_t { CUR_PLN = 0, CUR_EUR, CUR_GBP, CUR_USD };

struct Settings {
    char     ssid[33];
    char     pass[65];
    char     sig_ip[16];
    uint8_t  inv_id;             // Modbus id of the inverter (default 1)
    float    import_price;       // PLN/kWh gross (energy + variable distribution)
    float    export_coef;        // 1.23 (net-billing, since 02.2025)
    bool     neg_as_zero;        // negative RCE counts as 0
    uint8_t  lang;               // LANG_PL / LANG_EN
    uint8_t  earn_mode;          // EARN_TOTAL / EARN_DEPOSIT
    uint8_t  price_src;          // SRC_RCE / SRC_EC / SRC_FIXED
    uint8_t  price_res;          // RES_HOUR / RES_QUARTER
    uint8_t  zone;               // index in SIG_ZONES (Energy-Charts zone + time zone)
    float    fx_rate;            // EUR -> display currency (Energy-Charts prices are EUR)
    float    fixed_export_price; // per kWh, used with SRC_FIXED (other markets)
    float    energy_price;       // per kWh gross, energy part only (paid from the deposit)
    uint8_t  currency;           // CUR_*
    uint8_t  lcd_clk;            // display pixel clock: 0 = 16 MHz, 1 = 15 MHz, 2 = 14 MHz (stored as MHz, key "lcdmhz")
    uint8_t  lcd_bb;             // bounce buffer: 0 = small (4 lines), 1 = medium (10 lines, default), 2 = off - needs restart
};
// 12 MHz turned out to be too low for the panel (white screen) - the range is 14..16 MHz
static inline uint8_t  sig_lcd_clk_mhz(uint8_t c) { return c == 0 ? 16 : (c == 1 ? 15 : 14); }
static inline uint32_t sig_lcd_clk_hz(uint8_t c)  { return sig_lcd_clk_mhz(c) * 1000000u; }
static inline uint8_t  sig_lcd_clk_idx(uint8_t mhz) { return mhz >= 16 ? 0 : (mhz == 15 ? 1 : 2); }
#define SIG_LCD_CLK_DEFAULT 0      // 16 MHz (Waveshare default, best in tests)

void sig_lcd_defaults();          // web recovery: 16 MHz, medium buffer, saved

// ---------------- API ----------------
#define SIGDASH_VERSION "1.6.1"
#ifndef SIGDASH_BUILD
#define SIGDASH_BUILD "Arduino"       // "XIP" = GitHub build with code in PSRAM (see platformio.ini)
#endif
// Author shown in "About", on the web page and in the Excel export
#define SIGDASH_AUTHOR  "Marcin Janczarek"
#define SIGDASH_CONTACT "sigdashmj@gmail.com"

void sig_backend_begin();                 // call once in setup()
void sig_mem_report(const char *tag);     // internal / PSRAM usage to Serial

void sig_get_data(SigData &out);
void sig_get_prices(PriceData &out);
void sig_get_money(MoneyData &out);
void sig_get_status(NetStatus &out);
void sig_get_settings(Settings &out);
void sig_get_soc_history(uint8_t out[96]);   // today's SOC per 15 min, 255 = no data
struct DayRec;
void sig_get_today_dayrec(DayRec &out);      // today so far, for the export

// requests from the UI (thread safe, non-blocking)
void sig_req_wifi_scan();
void sig_req_wifi_connect(const char *ssid, const char *pass);
void sig_req_discover();
void sig_req_set_ip(const char *ip);
void sig_req_set_inv_id(uint8_t id);
void sig_req_set_price(float import_price);
void sig_req_set_coef(float coef);
void sig_req_set_neg_zero(bool on);
void sig_req_reset_money();
void sig_req_update_prefs(const Settings &s);   // lang, modes, prices, currency, inverter id
void sig_req_set_deposit(double value);

const char *sig_currency(uint8_t c);

// helpers shared with the UI
const char *sig_ems_mode_text(uint16_t m, uint8_t lang);
const char *sig_run_state_text(uint16_t s, uint8_t lang);

// exposed for host tests
int  pse_parse_quarters(const char *json, float *q96);     // returns quarter count
int  ec_parse_day(const char *json, const char *date, float *q96);  // Energy-Charts, local date YYYY-MM-DD
const char *sig_zone_tz(const Settings &s);               // time zone used by the device
void pse_quarters_to_hours(const float *q96, float *h24);
