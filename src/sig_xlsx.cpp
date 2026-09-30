// ============================================================
// SigDash - streaming XLSX writer (see sig_xlsx.h)
// ============================================================

#include "sig_xlsx.h"
#include <time.h>
#include <math.h>

// ------------------------------------------------------------
// CRC32 (zip / zlib polynomial)
// ------------------------------------------------------------
static uint32_t crc_table[256];

static void crc_init()
{
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

class MeasureSink : public Sink {
public:
    uint32_t crc = 0xFFFFFFFFu, size = 0;
    void put(const char *s, size_t n) override
    {
        for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ (uint8_t)s[i]) & 0xFF] ^ (crc >> 8);
        size += n;
    }
    uint32_t result() const { return crc ^ 0xFFFFFFFFu; }
};

// ------------------------------------------------------------
// small XML helpers
// ------------------------------------------------------------
static void xml_text(Sink &o, const char *s)
{
    char buf[64];
    size_t n = 0;
    for (; *s; s++) {
        const char *rep = nullptr;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (n + l > sizeof(buf)) { o.put(buf, n); n = 0; }
            memcpy(buf + n, rep, l);
            n += l;
        } else {
            if (n + 1 > sizeof(buf)) { o.put(buf, n); n = 0; }
            buf[n++] = *s;
        }
    }
    if (n) o.put(buf, n);
}

// styles (see styles.xml below)
enum { ST_NONE = 0, ST_HEAD, ST_DATE, ST_DATETIME, ST_2DEC, ST_3DEC, ST_PCT, ST_BOLD, ST_1DEC, ST_INT };

static void c_str(Sink &o, const char *s, int style = ST_NONE)
{
    char b[48];
    snprintf(b, sizeof(b), style ? "<c t=\"inlineStr\" s=\"%d\"><is><t>" : "<c t=\"inlineStr\"><is><t>", style);
    o.str(b);
    xml_text(o, s);
    o.str("</t></is></c>");
}

static void c_num(Sink &o, double v, int style)
{
    if (isnan(v) || isinf(v)) { o.str("<c/>"); return; }
    char b[64];
    // enough precision, no exponent, trailing zeros trimmed
    char num[32];
    snprintf(num, sizeof(num), style == ST_DATETIME ? "%.9f" : "%.6f", v);
    char *e = num + strlen(num) - 1;
    while (e > num && *e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
    if (strcmp(num, "-0") == 0) strcpy(num, "0");
    snprintf(b, sizeof(b), "<c s=\"%d\"><v>%s</v></c>", style, num);
    o.str(b);
}

static void row_begin(Sink &o) { o.str("<row>"); }
static void row_end(Sink &o) { o.str("</row>"); }

static double excel_date(uint32_t yyyymmdd)
{
    return days_from_civil(yyyymmdd / 10000, (yyyymmdd / 100) % 100, yyyymmdd % 100) + 25569.0;
}

static double excel_datetime(time_t t)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    return days_from_civil(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday) + 25569.0 +
           (tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec) / 86400.0;
}

static const char *cur_name(uint8_t c)
{
    switch (c) {
    case CUR_EUR: return "EUR";
    case CUR_GBP: return "GBP";
    case CUR_USD: return "USD";
    default:      return "PLN";
    }
}

#define L(pl, en) (r.set.lang == LANG_EN ? (en) : (pl))

// ------------------------------------------------------------
// static parts of the package
// ------------------------------------------------------------
static const char *CONTENT_TYPES =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
    "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
    "<Override PartName=\"/xl/worksheets/sheet2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
    "<Override PartName=\"/xl/worksheets/sheet3.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
    "<Override PartName=\"/xl/worksheets/sheet4.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
    "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
    "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
    "</Types>";

static const char *ROOT_RELS =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
    "</Relationships>";

static const char *APP_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
    "<Application>SigDash</Application></Properties>";

static const char *WB_RELS =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet2.xml\"/>"
    "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet3.xml\"/>"
    "<Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet4.xml\"/>"
    "<Relationship Id=\"rId5\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
    "</Relationships>";

// cellXfs index = ST_* enum
static const char *STYLES =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<numFmts count=\"4\">"
    "<numFmt numFmtId=\"164\" formatCode=\"yyyy-mm-dd\"/>"
    "<numFmt numFmtId=\"165\" formatCode=\"yyyy-mm-dd hh:mm\"/>"
    "<numFmt numFmtId=\"166\" formatCode=\"0.000\"/>"
    "<numFmt numFmtId=\"167\" formatCode=\"0.0\"/>"
    "</numFmts>"
    "<fonts count=\"2\">"
    "<font><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font>"
    "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font>"
    "</fonts>"
    "<fills count=\"3\">"
    "<fill><patternFill patternType=\"none\"/></fill>"
    "<fill><patternFill patternType=\"gray125\"/></fill>"
    "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFDDEBF7\"/><bgColor indexed=\"64\"/></patternFill></fill>"
    "</fills>"
    "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
    "<cellXfs count=\"10\">"
    "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
    "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"0\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyAlignment=\"1\"><alignment wrapText=\"1\" vertical=\"center\"/></xf>"
    "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"165\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"2\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"166\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"9\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>"
    "<xf numFmtId=\"167\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"1\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "</cellXfs>"
    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
    "</styleSheet>";

static void gen_workbook(const ExportReq &r, Sink &o)
{
    o.str("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
          "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
          "<bookViews><workbookView/></bookViews><sheets>");
    const char *names[4] = {L("Podsumowanie", "Summary"), L("Dni", "Days"),
                            L("Kwadranse", "Quarters"), L("Informacje", "Info")};
    for (int i = 0; i < 4; i++) {
        char b[128];
        snprintf(b, sizeof(b), "<sheet name=\"%s\" sheetId=\"%d\" r:id=\"rId%d\"/>", names[i], i + 1, i + 1);
        o.str(b);
    }
    o.str("</sheets></workbook>");
}

// ------------------------------------------------------------
// sheet frame
// ------------------------------------------------------------
static void sheet_begin(Sink &o, const float *widths, int ncols, bool freeze)
{
    o.str("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");
    if (freeze)
        o.str("<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" "
              "activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>");
    else
        o.str("<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>");
    o.str("<cols>");
    for (int i = 0; i < ncols; i++) {
        char b[96];
        snprintf(b, sizeof(b), "<col min=\"%d\" max=\"%d\" width=\"%.1f\" customWidth=\"1\"/>", i + 1, i + 1, widths[i]);
        o.str(b);
    }
    o.str("</cols><sheetData>");
}

static void sheet_end(Sink &o) { o.str("</sheetData></worksheet>"); }

static void header_row(Sink &o, const char *const *h, int n)
{
    row_begin(o);
    for (int i = 0; i < n; i++) c_str(o, h[i], ST_HEAD);
    row_end(o);
}

// ------------------------------------------------------------
// data access (file records + live today row)
// ------------------------------------------------------------
struct DayCtx {
    Sink *o;
    const ExportReq *r;
    // totals
    double pv, load, imp, exp_, bc, bd, saved, expv, impc, dep_in, dep_out, deposit_end;
    double price_sum; int price_n;
    uint32_t last_date;
    int n;
};

static void day_accumulate(DayCtx &c, const DayRec &d)
{
    c.pv += d.kwh_pv;   c.load += d.kwh_load;  c.imp += d.kwh_imp;  c.exp_ += d.kwh_exp;
    c.bc += d.kwh_bchg; c.bd += d.kwh_bdis;
    c.saved += d.saved; c.expv += d.exp_val;   c.impc += d.imp_cost;
    c.dep_in += d.dep_in; c.dep_out += d.dep_out;
    if (d.date >= c.last_date) { c.last_date = d.date; c.deposit_end = d.deposit_end; }
    if (!isnan(d.price_avg)) { c.price_sum += d.price_avg; c.price_n++; }
    c.n++;
}

static void for_each_day(const ExportReq &r, void (*fn)(const DayRec &, void *), void *ctx)
{
    // stored days before "today" of the request, then the live today row
    uint32_t to = r.to_date < r.today_date ? r.to_date : r.today_date - 1;
    if (r.from_date <= to) store_read_days(r.from_date, to, fn, ctx);
    if (r.today_date >= r.from_date && r.today_date <= r.to_date) fn(r.today, ctx);
}

static void acc_cb(const DayRec &d, void *ctx) { day_accumulate(*(DayCtx *)ctx, d); }

// ------------------------------------------------------------
// sheet 1: summary
// ------------------------------------------------------------
static void kv(Sink &o, const char *k, double v, int style, const char *unit)
{
    row_begin(o);
    c_str(o, k, ST_BOLD);
    c_num(o, v, style);
    c_str(o, unit);
    row_end(o);
}

static void gen_summary(const ExportReq &r, Sink &o)
{
    DayCtx c;
    memset(&c, 0, sizeof(c));
    c.deposit_end = NAN;
    for_each_day(r, acc_cb, &c);

    const float w[3] = {42, 16, 14};
    sheet_begin(o, w, 3, false);
    const char *cur = cur_name(r.set.currency);
    char per_kwh[16];
    snprintf(per_kwh, sizeof(per_kwh), "%s/kWh", cur);

    row_begin(o);
    c_str(o, L("SigDash – podsumowanie", "SigDash – summary"), ST_BOLD);
    row_end(o);
    row_begin(o);
    c_str(o, L("Od", "From"), ST_BOLD); c_num(o, excel_date(r.from_date), ST_DATE);
    row_end(o);
    row_begin(o);
    c_str(o, L("Do", "To"), ST_BOLD); c_num(o, excel_date(r.to_date), ST_DATE);
    row_end(o);
    row_begin(o);
    c_str(o, L("Dni z danymi", "Days with data"), ST_BOLD); c_num(o, c.n, ST_INT);
    row_end(o);
    row_begin(o); row_end(o);

    row_begin(o); c_str(o, L("Energia", "Energy"), ST_HEAD); c_str(o, "", ST_HEAD); c_str(o, "", ST_HEAD); row_end(o);
    kv(o, L("Produkcja PV", "Solar production"), c.pv, ST_2DEC, "kWh");
    kv(o, L("Zużycie domu", "Home consumption"), c.load, ST_2DEC, "kWh");
    kv(o, L("Pobór z sieci", "Grid import"), c.imp, ST_2DEC, "kWh");
    kv(o, L("Oddanie do sieci", "Grid export"), c.exp_, ST_2DEC, "kWh");
    kv(o, L("Ładowanie baterii", "Battery charge"), c.bc, ST_2DEC, "kWh");
    kv(o, L("Rozładowanie baterii", "Battery discharge"), c.bd, ST_2DEC, "kWh");
    kv(o, L("Autarkia (zużycie pokryte bez sieci)", "Self-sufficiency (use covered without grid)"),
       c.load > 0 ? 1.0 - c.imp / c.load : NAN, ST_PCT, "");
    kv(o, L("Autokonsumpcja (produkcja zużyta na miejscu)", "Self-consumption (production used on site)"),
       c.pv > 0 ? 1.0 - c.exp_ / c.pv : NAN, ST_PCT, "");
    row_begin(o); row_end(o);

    row_begin(o); c_str(o, L("Pieniądze", "Money"), ST_HEAD); c_str(o, "", ST_HEAD); c_str(o, "", ST_HEAD); row_end(o);
    kv(o, L("Oszczędność (autokonsumpcja × cena zakupu)", "Savings (self-use × import price)"), c.saved, ST_2DEC, cur);
    kv(o, L("Wartość sprzedaży", "Export value"), c.expv, ST_2DEC, cur);
    kv(o, L("Zarobek łącznie", "Total earnings"), c.saved + c.expv, ST_2DEC, cur);
    kv(o, L("Koszt zakupu z sieci", "Grid import cost"), c.impc, ST_2DEC, cur);
    kv(o, L("Średnia ważona cena sprzedaży", "Weighted average export price"),
       c.exp_ > 0 ? c.expv / c.exp_ : NAN, ST_3DEC, per_kwh);
    kv(o, L("Średnia cena rynkowa (średnia dni)", "Average market price (mean of days)"),
       c.price_n ? c.price_sum / c.price_n : NAN, ST_3DEC, per_kwh);
    row_begin(o); row_end(o);

    row_begin(o); c_str(o, L("Konto prosumenta", "Prosumer account"), ST_HEAD); c_str(o, "", ST_HEAD); c_str(o, "", ST_HEAD); row_end(o);
    kv(o, L("Wpływy (sprzedaż)", "Paid in (export)"), c.dep_in, ST_2DEC, cur);
    kv(o, L("Pobrania (energia z sieci)", "Paid out (grid energy)"), c.dep_out, ST_2DEC, cur);
    kv(o, L("Zmiana netto", "Net change"), c.dep_in - c.dep_out, ST_2DEC, cur);
    kv(o, L("Stan na koniec okresu", "Balance at the end"), c.deposit_end, ST_2DEC, cur);

    sheet_end(o);
}

// ------------------------------------------------------------
// sheet 2: days
// ------------------------------------------------------------
static void day_row_cb(const DayRec &d, void *ctx)
{
    Sink &o = *((DayCtx *)ctx)->o;
    row_begin(o);
    c_num(o, excel_date(d.date), ST_DATE);
    c_num(o, d.kwh_pv, ST_2DEC);   c_num(o, d.kwh_load, ST_2DEC);
    c_num(o, d.kwh_imp, ST_2DEC);  c_num(o, d.kwh_exp, ST_2DEC);
    c_num(o, d.kwh_bchg, ST_2DEC); c_num(o, d.kwh_bdis, ST_2DEC);
    c_num(o, d.soc_min, ST_INT);   c_num(o, d.soc_max, ST_INT);
    c_num(o, d.pv_peak_kw, ST_2DEC); c_num(o, d.imp_peak_kw, ST_2DEC);
    c_num(o, d.kwh_load > 0 ? 1.0 - d.kwh_imp / d.kwh_load : NAN, ST_PCT);
    c_num(o, d.saved, ST_2DEC);    c_num(o, d.exp_val, ST_2DEC);
    c_num(o, d.saved + d.exp_val, ST_2DEC);
    c_num(o, d.imp_cost, ST_2DEC);
    c_num(o, d.dep_in, ST_2DEC);   c_num(o, d.dep_out, ST_2DEC);  c_num(o, d.deposit_end, ST_2DEC);
    c_num(o, d.kwh_exp > 0 ? d.exp_val / d.kwh_exp : NAN, ST_3DEC);
    c_num(o, d.price_avg, ST_3DEC);
    row_end(o);
}

static void gen_days(const ExportReq &r, Sink &o)
{
    const float w[21] = {12, 10, 10, 10, 10, 10, 10, 9, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 12, 12};
    sheet_begin(o, w, 21, true);
    const char *cur = cur_name(r.set.currency);
    static char h[21][48];
    const char *pl[21] = {"Data", "PV kWh", "Zużycie kWh", "Pobór kWh", "Oddanie kWh", "Ładowanie kWh", "Rozładowanie kWh",
                          "SOC min %%", "SOC max %%", "Szczyt PV kW", "Szczyt poboru kW", "Autarkia",
                          "Oszczędność %s", "Sprzedaż %s", "Zarobek %s", "Koszt zakupu %s",
                          "Depozyt wpływ %s", "Depozyt pobór %s", "Depozyt stan %s",
                          "Śr. cena sprzedaży %s/kWh", "Śr. cena rynkowa %s/kWh"};
    const char *en[21] = {"Date", "Solar kWh", "Home kWh", "Import kWh", "Export kWh", "Batt. charge kWh", "Batt. discharge kWh",
                          "SOC min %%", "SOC max %%", "Solar peak kW", "Import peak kW", "Self-sufficiency",
                          "Savings %s", "Export %s", "Earnings %s", "Import cost %s",
                          "Deposit in %s", "Deposit out %s", "Deposit balance %s",
                          "Avg. export price %s/kWh", "Avg. market price %s/kWh"};
    const char *hp[21];
    for (int i = 0; i < 21; i++) {
        snprintf(h[i], sizeof(h[i]), r.set.lang == LANG_EN ? en[i] : pl[i], cur);
        hp[i] = h[i];
    }
    header_row(o, hp, 21);

    DayCtx c;
    memset(&c, 0, sizeof(c));
    c.o = &o;
    c.r = &r;
    for_each_day(r, day_row_cb, &c);
    sheet_end(o);
}

// ------------------------------------------------------------
// sheet 3: quarters
// ------------------------------------------------------------
static void q_row_cb(const QRec &q, void *ctx)
{
    Sink &o = *(Sink *)ctx;
    row_begin(o);
    c_num(o, excel_datetime((time_t)q.ts), ST_DATETIME);
    c_num(o, q.price_market, ST_3DEC);
    c_num(o, q.price_export, ST_3DEC);
    c_num(o, q.pv_wh / 1000.0, ST_3DEC);   c_num(o, q.load_wh / 1000.0, ST_3DEC);
    c_num(o, q.imp_wh / 1000.0, ST_3DEC);  c_num(o, q.exp_wh / 1000.0, ST_3DEC);
    c_num(o, q.bchg_wh / 1000.0, ST_3DEC); c_num(o, q.bdis_wh / 1000.0, ST_3DEC);
    c_num(o, q.soc, ST_INT);
    c_num(o, q.saved, ST_3DEC);  c_num(o, q.exp_val, ST_3DEC);
    c_num(o, q.imp_cost, ST_3DEC); c_num(o, q.dep_out, ST_3DEC);
    row_end(o);
}

static void gen_quarters(const ExportReq &r, Sink &o)
{
    const float w[14] = {17, 11, 11, 9, 9, 9, 9, 9, 9, 7, 10, 10, 10, 10};
    sheet_begin(o, w, 14, true);
    const char *cur = cur_name(r.set.currency);
    static char h[14][48];
    const char *pl[14] = {"Początek kwadransu", "Cena rynkowa %s/kWh", "Cena sprzedaży %s/kWh", "PV kWh", "Zużycie kWh",
                          "Pobór kWh", "Oddanie kWh", "Ładowanie kWh", "Rozładowanie kWh", "SOC %%",
                          "Oszczędność %s", "Sprzedaż %s", "Koszt zakupu %s", "Depozyt pobór %s"};
    const char *en[14] = {"Quarter start", "Market price %s/kWh", "Export price %s/kWh", "Solar kWh", "Home kWh",
                          "Import kWh", "Export kWh", "Batt. charge kWh", "Batt. discharge kWh", "SOC %%",
                          "Savings %s", "Export %s", "Import cost %s", "Deposit out %s"};
    const char *hp[14];
    for (int i = 0; i < 14; i++) {
        snprintf(h[i], sizeof(h[i]), r.set.lang == LANG_EN ? en[i] : pl[i], cur);
        hp[i] = h[i];
    }
    header_row(o, hp, 14);

    if (r.with_quarters) {
        int y = r.from_date / 10000, m = (r.from_date / 100) % 100, d = r.from_date % 100;
        struct tm a = {};
        a.tm_year = y - 1900; a.tm_mon = m - 1; a.tm_mday = d; a.tm_isdst = -1;
        time_t from_ts = mktime(&a);
        y = r.to_date / 10000; m = (r.to_date / 100) % 100; d = r.to_date % 100;
        struct tm b = {};
        b.tm_year = y - 1900; b.tm_mon = m - 1; b.tm_mday = d + 1; b.tm_isdst = -1;
        time_t to_ts = mktime(&b) - 1;
        if ((uint32_t)to_ts > r.q_to_ts) to_ts = r.q_to_ts;
        store_read_quarters((uint32_t)from_ts, (uint32_t)to_ts, q_row_cb, &o);
    } else {
        row_begin(o);
        char t[96];
        snprintf(t, sizeof(t), L("Zakres dłuższy niż %d dni – kwadranse pominięto. Wybierz krótszy zakres.",
                                 "Range longer than %d days – quarters left out. Choose a shorter range."),
                 XLSX_MAX_QUARTER_DAYS);
        c_str(o, t);
        row_end(o);
    }
    sheet_end(o);
}

// ------------------------------------------------------------
// sheet 4: info
// ------------------------------------------------------------
static void info_row(Sink &o, const char *k, const char *v)
{
    row_begin(o);
    c_str(o, k, ST_BOLD);
    c_str(o, v);
    row_end(o);
}

static void gen_info(const ExportReq &r, Sink &o)
{
    const float w[2] = {34, 70};
    sheet_begin(o, w, 2, false);
    char b[128];

    info_row(o, L("Wygenerowano", "Generated"), r.generated);
    snprintf(b, sizeof(b), "%04u-%02u-%02u – %04u-%02u-%02u",
             (unsigned)(r.from_date / 10000), (unsigned)(r.from_date / 100 % 100), (unsigned)(r.from_date % 100),
             (unsigned)(r.to_date / 10000), (unsigned)(r.to_date / 100 % 100), (unsigned)(r.to_date % 100));
    info_row(o, L("Zakres", "Range"), b);

    const char *src = r.set.price_src == SRC_RCE ? "PSE RCE (Polska)"
                    : r.set.price_src == SRC_EC ? "Energy-Charts (day-ahead)"
                                                : L("Stała cena", "Fixed price");
    info_row(o, L("Źródło cen (obecnie)", "Price source (current)"), src);
    if (r.set.price_src == SRC_EC) info_row(o, L("Strefa cenowa", "Bidding zone"), SIG_ZONES[r.set.zone].code);
    info_row(o, L("Rozdzielczość ceny", "Price resolution"), r.set.price_res == RES_QUARTER ? "15 min" : "1 h");
    info_row(o, L("Waluta", "Currency"), cur_name(r.set.currency));
    snprintf(b, sizeof(b), "%.2f", r.set.import_price);
    info_row(o, L("Cena zakupu z dystrybucją (obecnie)", "Import price incl. grid fees (current)"), b);
    snprintf(b, sizeof(b), "%.2f", r.set.energy_price);
    info_row(o, L("Cena samej energii (obecnie)", "Energy-only price (current)"), b);
    if (r.set.price_src == SRC_RCE) info_row(o, L("Współczynnik sprzedaży", "Export coefficient"), r.set.export_coef > 1.01f ? "1.23" : "1.00");
    info_row(o, L("Uwaga", "Note"),
             L("Każdy kwadrans i dzień zapisano z cenami obowiązującymi w chwili pomiaru – zmiana ustawień nie przelicza historii.",
               "Every quarter and day is stored with the prices valid at that time – changing settings does not recalculate history."));
    info_row(o, L("Nośnik danych", "Storage"), r.si.medium);
    info_row(o, L("Źródło danych instalacji", "Plant data"), "Sigenergy Modbus TCP (read-only)");
    if (r.set.price_src == SRC_EC)
        info_row(o, L("Licencja cen", "Price licence"), "energy-charts.info – CC BY 4.0 (ENTSO-E, Bundesnetzagentur | SMARD.de)");
    info_row(o, L("Program", "Software"), "SigDash v" SIGDASH_VERSION);
    info_row(o, L("Autor", "Author"), SIGDASH_AUTHOR);
    info_row(o, L("Kontakt", "Contact"), SIGDASH_CONTACT);
    info_row(o, L("Zastrzeżenie", "Disclaimer"),
             L("Nieoficjalne narzędzie, niezwiązane z Sigenergy. Wyliczenia są szacunkowe – wiążąca jest faktura sprzedawcy energii.",
               "Unofficial tool, not affiliated with Sigenergy. Figures are estimates – your supplier's bill is binding."));
    sheet_end(o);
}

// ------------------------------------------------------------
// package
// ------------------------------------------------------------
enum { E_CT, E_RELS, E_APP, E_WB, E_WBRELS, E_STYLES, E_S1, E_S2, E_S3, E_S4, E_COUNT };

static const char *ENTRY_NAMES[E_COUNT] = {
    "[Content_Types].xml", "_rels/.rels", "docProps/app.xml", "xl/workbook.xml", "xl/_rels/workbook.xml.rels",
    "xl/styles.xml", "xl/worksheets/sheet1.xml", "xl/worksheets/sheet2.xml",
    "xl/worksheets/sheet3.xml", "xl/worksheets/sheet4.xml"
};

static uint32_t e_crc[E_COUNT], e_size[E_COUNT];

static void gen_entry(int i, const ExportReq &r, Sink &o)
{
    switch (i) {
    case E_CT:     o.str(CONTENT_TYPES); break;
    case E_RELS:   o.str(ROOT_RELS); break;
    case E_APP:    o.str(APP_XML); break;
    case E_WB:     gen_workbook(r, o); break;
    case E_WBRELS: o.str(WB_RELS); break;
    case E_STYLES: o.str(STYLES); break;
    case E_S1:     gen_summary(r, o); break;
    case E_S2:     gen_days(r, o); break;
    case E_S3:     gen_quarters(r, o); break;
    case E_S4:     gen_info(r, o); break;
    }
}

static void le16(Sink &o, uint16_t v) { char b[2] = {(char)(v & 0xFF), (char)(v >> 8)}; o.put(b, 2); }
static void le32(Sink &o, uint32_t v)
{
    char b[4] = {(char)(v & 0xFF), (char)((v >> 8) & 0xFF), (char)((v >> 16) & 0xFF), (char)(v >> 24)};
    o.put(b, 4);
}

static uint16_t dos_time, dos_date;

static void local_header(Sink &o, int i)
{
    le32(o, 0x04034b50);
    le16(o, 20);             // version needed
    le16(o, 0x0800);         // UTF-8 names
    le16(o, 0);              // stored
    le16(o, dos_time);
    le16(o, dos_date);
    le32(o, e_crc[i]);
    le32(o, e_size[i]);
    le32(o, e_size[i]);
    le16(o, strlen(ENTRY_NAMES[i]));
    le16(o, 0);
    o.str(ENTRY_NAMES[i]);
}

void xlsx_prepare(ExportReq &r, uint32_t from_date, uint32_t to_date, time_t now)
{
    memset(&r, 0, sizeof(r));
    if (from_date > to_date) { uint32_t t = from_date; from_date = to_date; to_date = t; }
    r.from_date = from_date;
    r.to_date = to_date;
    r.today_date = date_yyyymmdd(now);
    r.q_to_ts = (uint32_t)(now / 900) * 900 - 1;        // only finished quarters
    int32_t span = days_from_civil(to_date / 10000, to_date / 100 % 100, to_date % 100) -
                   days_from_civil(from_date / 10000, from_date / 100 % 100, from_date % 100) + 1;
    r.with_quarters = span <= XLSX_MAX_QUARTER_DAYS;
    sig_get_settings(r.set);
    sig_get_today_dayrec(r.today);
    store_info(r.si);
    struct tm t;
    localtime_r(&now, &t);
    strftime(r.generated, sizeof(r.generated), "%Y-%m-%d %H:%M", &t);
    dos_time = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2);
    dos_date = ((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday;
}

uint32_t xlsx_measure(ExportReq &r)
{
    crc_init();
    uint32_t total = 0, cd = 0;
    for (int i = 0; i < E_COUNT; i++) {
        MeasureSink m;
        gen_entry(i, r, m);
        e_crc[i] = m.result();
        e_size[i] = m.size;
        total += 30 + strlen(ENTRY_NAMES[i]) + m.size;
        cd += 46 + strlen(ENTRY_NAMES[i]);
    }
    return total + cd + 22;
}

void xlsx_write(ExportReq &r, Sink &o)
{
    uint32_t offs[E_COUNT], pos = 0;
    for (int i = 0; i < E_COUNT; i++) {
        offs[i] = pos;
        local_header(o, i);
        gen_entry(i, r, o);
        pos += 30 + strlen(ENTRY_NAMES[i]) + e_size[i];
    }
    uint32_t cd_start = pos, cd_size = 0;
    for (int i = 0; i < E_COUNT; i++) {
        le32(o, 0x02014b50);
        le16(o, 20);         // made by
        le16(o, 20);         // needed
        le16(o, 0x0800);
        le16(o, 0);
        le16(o, dos_time);
        le16(o, dos_date);
        le32(o, e_crc[i]);
        le32(o, e_size[i]);
        le32(o, e_size[i]);
        le16(o, strlen(ENTRY_NAMES[i]));
        le16(o, 0); le16(o, 0); le16(o, 0); le16(o, 0);
        le32(o, 0);
        le32(o, offs[i]);
        o.str(ENTRY_NAMES[i]);
        cd_size += 46 + strlen(ENTRY_NAMES[i]);
    }
    le32(o, 0x06054b50);
    le16(o, 0); le16(o, 0);
    le16(o, E_COUNT); le16(o, E_COUNT);
    le32(o, cd_size);
    le32(o, cd_start);
    le16(o, 0);
}
