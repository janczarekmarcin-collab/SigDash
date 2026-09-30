// ============================================================
// SigDash - web server with the Excel export (see sig_web.h)
// ============================================================

#include "sig_web.h"
#include "sig_backend.h"
#include "sig_store.h"
#include "sig_xlsx.h"

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static WebServer server(80);

static const char *RANGE_KEYS[RANGE_COUNT] = {"today", "yesterday", "month", "prevmonth", "year", "all"};

// ------------------------------------------------------------
// ranges
// ------------------------------------------------------------
bool sig_range_dates(int range, uint32_t &from, uint32_t &to)
{
    time_t now = time(nullptr);
    if (now < 1700000000) return false;
    struct tm t;
    localtime_r(&now, &t);
    uint32_t today = date_yyyymmdd(now);
    int y = t.tm_year + 1900, m = t.tm_mon + 1;

    switch (range) {
    case RANGE_TODAY:
        from = to = today;
        break;
    case RANGE_YESTERDAY:
        from = to = date_yyyymmdd(now - 24 * 3600);
        break;
    case RANGE_MONTH:
        from = y * 10000 + m * 100 + 1;
        to = today;
        break;
    case RANGE_PREV_MONTH: {
        int py = m == 1 ? y - 1 : y, pm = m == 1 ? 12 : m - 1;
        static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int last = dim[pm - 1] + ((pm == 2 && ((py % 4 == 0 && py % 100 != 0) || py % 400 == 0)) ? 1 : 0);
        from = py * 10000 + pm * 100 + 1;
        to = py * 10000 + pm * 100 + last;
        break;
    }
    case RANGE_YEAR:
        from = y * 10000 + 101;
        to = today;
        break;
    default: {
        StoreInfo si;
        store_info(si);
        from = si.first_date ? si.first_date : today;
        to = today;
        break;
    }
    }
    return true;
}

bool sig_export_url(char *buf, size_t n, int range)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    snprintf(buf, n, "http://%s/x?r=%s", WiFi.localIP().toString().c_str(),
             RANGE_KEYS[range < 0 || range >= RANGE_COUNT ? RANGE_MONTH : range]);
    return true;
}

// ------------------------------------------------------------
// streaming to the HTTP client
// ------------------------------------------------------------
class NetSink : public Sink {
public:
    void put(const char *s, size_t n) override
    {
        while (n) {
            size_t k = min(n, sizeof(buf) - len);
            memcpy(buf + len, s, k);
            len += k; s += k; n -= k;
            if (len == sizeof(buf)) flush();
        }
    }
    void flush()
    {
        if (len && !failed) {
            if (server.client().write(buf, len) != len) failed = true;
        }
        len = 0;
    }
    bool failed = false;
private:
    uint8_t buf[2048];
    size_t len = 0;
};

static uint32_t parse_date(const String &s)
{
    int y, m, d;
    if (sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
    if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    return y * 10000 + m * 100 + d;
}

static void handle_export()
{
    uint32_t from = 0, to = 0;
    if (server.hasArg("from") && server.hasArg("to")) {
        from = parse_date(server.arg("from"));
        to = parse_date(server.arg("to"));
    } else {
        int range = RANGE_MONTH;
        String r = server.arg("r");
        for (int i = 0; i < RANGE_COUNT; i++) if (r == RANGE_KEYS[i]) range = i;
        sig_range_dates(range, from, to);
    }
    if (!from || !to) {
        server.send(400, "text/plain", "Bad date range / zly zakres dat");
        return;
    }

    static ExportReq req;                       // big struct - keep it off the stack
    xlsx_prepare(req, from, to, time(nullptr));
    uint32_t t0 = millis();
    uint32_t total = xlsx_measure(req);

    char fn[96];
    snprintf(fn, sizeof(fn), "attachment; filename=\"SigDash_%04u-%02u-%02u_%04u-%02u-%02u.xlsx\"",
             (unsigned)(req.from_date / 10000), (unsigned)(req.from_date / 100 % 100), (unsigned)(req.from_date % 100),
             (unsigned)(req.to_date / 10000), (unsigned)(req.to_date / 100 % 100), (unsigned)(req.to_date % 100));
    server.sendHeader("Content-Disposition", fn);
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(total);
    server.send(200, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "");

    NetSink out;
    xlsx_write(req, out);
    out.flush();
    Serial.printf("[WEB] export %u-%u: %u bytes in %u ms%s\n", (unsigned)req.from_date, (unsigned)req.to_date,
                  (unsigned)total, (unsigned)(millis() - t0), out.failed ? " (client disconnected)" : "");
}

static void handle_root()
{
    Settings s;
    sig_get_settings(s);
    bool en = s.lang == LANG_EN;
    StoreInfo si;
    store_info(si);

    String h;
    h.reserve(3000);
    h += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>SigDash</title><style>"
         "body{font-family:system-ui,sans-serif;background:#0a0e15;color:#e9eef6;margin:0;padding:20px;max-width:560px}"
         "h1{font-size:22px;margin:0 0 4px}p{color:#8793a6;margin:4px 0 18px}"
         "a.b{display:block;background:#1a2331;border:1px solid #243044;color:#e9eef6;text-decoration:none;"
         "padding:14px 16px;border-radius:12px;margin:8px 0;font-size:17px}a.b:active{background:#4f8cff}"
         "form{background:#131a25;border:1px solid #243044;border-radius:12px;padding:14px;margin-top:16px}"
         "input,button{font-size:16px;padding:8px;border-radius:8px;border:1px solid #243044;background:#1a2331;color:#e9eef6}"
         "button{background:#4f8cff;border:0;margin-top:10px;width:100%}label{display:block;margin:6px 0;color:#8793a6}"
         "</style></head><body>";
    h += "<h1>SigDash – ";
    h += en ? "Excel export" : "eksport do Excela";
    h += "</h1><p>";
    h += en ? "Storage: " : "Nośnik: ";
    h += si.medium;
    h += en ? " · days recorded: " : " · zapisanych dni: ";
    h += String((unsigned)si.days);
    h += "</p>";

    const char *pl[RANGE_COUNT] = {"Dziś", "Wczoraj", "Ten miesiąc", "Poprzedni miesiąc", "Ten rok", "Wszystko"};
    const char *enl[RANGE_COUNT] = {"Today", "Yesterday", "This month", "Previous month", "This year", "Everything"};
    for (int i = 0; i < RANGE_COUNT; i++) {
        h += "<a class='b' href='/x?r=";
        h += RANGE_KEYS[i];
        h += "'>";
        h += en ? enl[i] : pl[i];
        h += " (.xlsx)</a>";
    }
    h += "<form action='/x'><label>";
    h += en ? "From" : "Od";
    h += "</label><input type='date' name='from' required><label>";
    h += en ? "To" : "Do";
    h += "</label><input type='date' name='to' required><button>";
    h += en ? "Download" : "Pobierz";
    h += "</button></form><p style='margin-top:16px'>";
    h += en ? "Ranges up to 93 days include the 15-minute sheet." : "Zakresy do 93 dni zawierają arkusz 15-minutowy.";
    h += "</p><p style='margin-top:28px;font-size:13px'>SigDash " SIGDASH_VERSION " · ";
    h += en ? "author: " : "autor: ";
    h += SIGDASH_AUTHOR;
    h += en ? " · unofficial, not affiliated with Sigenergy · read-only"
            : " · nieoficjalny, niezwiązany z Sigenergy · tylko odczyt";
    h += "</p></body></html>";
    server.send(200, "text/html; charset=utf-8", h);
}

// ------------------------------------------------------------
// task
// ------------------------------------------------------------
static void web_task(void *)
{
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));

    server.on("/", handle_root);
    server.on("/x", handle_export);
    server.on("/lcd-reset", []() {           // recovery if a display setting leaves the screen unusable
        sig_lcd_defaults();
        server.send(200, "text/plain; charset=utf-8", "SigDash: LCD 16 MHz, medium buffer - restarting / restart...");
        delay(800);
        esp_restart();
    });
    server.onNotFound([]() { server.send(404, "text/plain", "SigDash: not found"); });
    server.begin();
    Serial.printf("[WEB] http://%s/\n", WiFi.localIP().toString().c_str());

    for (;;) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void sig_web_begin()
{
    // no TLS here; 12 KB covers the XLSX writer (2 KB net buffer + record chunks)
    xTaskCreatePinnedToCore(web_task, "sig_web", 10240, nullptr, 1, nullptr, 0);
}
