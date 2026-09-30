#pragma once
// ============================================================
// SigDash - data history storage
//   SD card (if inserted at boot)  ->  /sigdash on the card
//   otherwise internal flash       ->  FFat ("ffat" partition) or LittleFS ("spiffs")
//
// Files:
//   /sigdash/days.bin        one DayRec per day, kept forever (~26 KB / year)
//   /sigdash/qYYYYMM.bin     QRec per 15 min, one file per month, kept 13 months
//
// All functions are thread safe (internal mutex).
// ============================================================

#include <Arduino.h>

#define STORE_KEEP_MONTHS 13

// One 15-minute interval (written when the quarter ends)
struct __attribute__((packed)) QRec {
    uint32_t ts;              // epoch of the quarter start
    float    price_market;    // [currency/kWh]
    float    price_export;    // value of 1 exported kWh (after coefficient / negative rule)
    uint16_t pv_wh, load_wh, imp_wh, exp_wh, bchg_wh, bdis_wh;
    uint8_t  soc;             // at the end of the quarter
    uint8_t  flags;           // bit0 price known, bits4..7 currency
    float    saved;           // self-consumption x import price
    float    exp_val;         // export x export price
    float    imp_cost;        // import x import price
    float    dep_out;         // import x energy price (paid from deposit)
};

// One day (written after midnight; today is built live from RAM)
struct __attribute__((packed)) DayRec {
    uint32_t date;            // yyyymmdd
    float    kwh_pv, kwh_load, kwh_imp, kwh_exp, kwh_bchg, kwh_bdis;
    float    saved, exp_val, imp_cost, dep_in, dep_out, deposit_end;
    uint8_t  soc_min, soc_max;
    uint8_t  currency, price_src;
    float    pv_peak_kw, imp_peak_kw;
    float    price_avg;       // time-weighted market price [currency/kWh]
    uint8_t  zone, reserved[3];
};

struct StoreInfo {
    bool     ok;
    char     medium[12];      // "SD", "Flash", "-"
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint32_t days;            // number of day records
    uint32_t first_date;      // yyyymmdd of the oldest day, 0 = none
};

// sd_cs_ready: the SD chip-select (CH422G EXIO4) has already been pulled low
bool store_begin(bool sd_cs_ready);

bool store_append_quarter(const QRec &q);
bool store_append_day(const DayRec &d);

// Iterate records in a range (inclusive). Records are read in small chunks;
// the store is NOT locked while callbacks run (they may send over the network).
typedef void (*day_cb_t)(const DayRec &d, void *ctx);
typedef void (*quarter_cb_t)(const QRec &q, void *ctx);
int store_read_days(uint32_t from_date, uint32_t to_date, day_cb_t cb, void *ctx);
int store_read_quarters(uint32_t from_ts, uint32_t to_ts, quarter_cb_t cb, void *ctx);

void store_info(StoreInfo &out);          // cheap snapshot (refreshed by store_refresh_info)
void store_refresh_info();                // reads the file system - call from the network task
                                          // (only at start and once a day: on internal flash every
                                          //  file-system access stalls PSRAM = display glitch)

// Small state files (e.g. counters) - ONLY on the SD card, returns false on internal flash.
bool store_is_sd();
bool store_write_blob(const char *name, const void *data, size_t len);
bool store_read_blob(const char *name, void *data, size_t len);

// helpers
uint32_t date_yyyymmdd(time_t t);         // local date
int32_t  days_from_civil(int y, int m, int d);   // days since 1970-01-01
