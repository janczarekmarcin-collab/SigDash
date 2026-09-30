#pragma once
// ============================================================
// SigDash - small web server in the home network:
//   http://<ip>/            page with export links
//   http://<ip>/x?r=month   download .xlsx (r = today, yesterday, month, prevmonth, year, all)
//   http://<ip>/x?from=2026-09-01&to=2026-09-30
// ============================================================

#include <Arduino.h>

enum ExportRange { RANGE_TODAY = 0, RANGE_YESTERDAY, RANGE_MONTH, RANGE_PREV_MONTH, RANGE_YEAR, RANGE_ALL, RANGE_COUNT };

void sig_web_begin();                                         // starts its own task
bool sig_export_url(char *buf, size_t n, int range);          // false if WiFi is not connected
bool sig_range_dates(int range, uint32_t &from, uint32_t &to); // yyyymmdd for a named range
