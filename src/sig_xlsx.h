#pragma once
// ============================================================
// SigDash - streaming XLSX writer (no compression, no RAM buffer
// for the whole file). The file is generated twice: first pass
// measures sizes + CRC32, second pass sends the bytes.
// ============================================================

#include <Arduino.h>
#include "sig_backend.h"
#include "sig_store.h"

#define XLSX_MAX_QUARTER_DAYS 93      // longer ranges: "Quarters" sheet is left out

class Sink {
public:
    virtual void put(const char *s, size_t n) = 0;
    void str(const char *s) { put(s, strlen(s)); }
    virtual ~Sink() {}
};

struct ExportReq {
    uint32_t  from_date, to_date;     // yyyymmdd inclusive
    uint32_t  today_date;             // yyyymmdd when the request started
    uint32_t  q_to_ts;                // quarters with ts <= this are exported
    bool      with_quarters;
    Settings  set;                    // prices, currency, language at export time
    DayRec    today;                  // live "today" row
    StoreInfo si;
    char      generated[20];          // "YYYY-MM-DD HH:MM"
};

// Fills the request for a range (dates yyyymmdd). now = current epoch.
void xlsx_prepare(ExportReq &r, uint32_t from_date, uint32_t to_date, time_t now);

// Total file size (pass 1). Must be called before xlsx_write.
uint32_t xlsx_measure(ExportReq &r);

// Writes the complete .xlsx (pass 2).
void xlsx_write(ExportReq &r, Sink &out);
