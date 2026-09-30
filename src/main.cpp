#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_err.h>
#include <esp_lcd_panel_rgb.h>
#include <Preferences.h>

#include <lvgl.h>
#include "esp_lv_adapter_arduino.h"
#include <math.h>
#include <time.h>

#include "sigfonts.h"
#include "sig_backend.h"
#include "sig_store.h"
#include "sig_web.h"
#include "qrcodegen.h"
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

using namespace esp_panel::drivers;
using namespace esp_panel::board;


// ============================================================
// SIGDASH v1.5
// Waveshare ESP32-S3-Touch-LCD-4.3B (800 x 480), LVGL 9.x
//
// Sigenergy (Modbus TCP po Wi-Fi, TYLKO ODCZYT) + ceny RCE z PSE + zarobek.
// Jezyki: polski / English.
//
// Pliki w folderze szkicu:
//   SigDash.ino, sig_backend.h/.cpp, sig_store.h/.cpp, sig_xlsx.h/.cpp,
//   sig_web.h/.cpp, qrcodegen.h/.c, sigfonts.h,
//   font_pl_14.c, font_pl_18.c, font_pl_24.c, font_pl_32.c, font_num_48.c,
//   esp_lv_adapter_arduino.h (+ pliki plytki, jak w KidGames)
//
// Arduino IDE: Tools > Partition Scheme > "16M Flash (3MB APP/9.9MB FATFS)"
//   (partycja na historie, gdy nie ma karty SD)
//
// Watki:
//   - zadanie sieciowe (core 0): Wi-Fi, Modbus, HTTPS, liczenie pieniedzy
//   - zadanie LVGL: wylacznie UI, dane bierze z kopii (sig_get_*)
// ============================================================


// ---------------- COLORS ------------------------------------

#define C_BG        0x0A0E15
#define C_CARD      0x131A25
#define C_CARD2     0x1A2331
#define C_BORDER    0x243044
#define C_TEXT      0xE9EEF6
#define C_DIM       0x8793A6
#define C_MUTED     0x566278
#define C_PV        0xFFC53D
#define C_IMPORT    0xFF6B6B
#define C_EXPORT    0x3DDC97
#define C_BATT      0x38BDF8
#define C_HOME      0xB794F6
#define C_GOLD      0xF5C451
#define C_ACCENT    0x4F8CFF


// ============================================================
// GLOBALS
// ============================================================

enum Page { PAGE_OVERVIEW = 0, PAGE_DETAILS, PAGE_BILLING, PAGE_SETTINGS, PAGE_COUNT };

static lv_obj_t *scr = nullptr;
static Page page = PAGE_OVERVIEW;

// snapshots (static: not on the LVGL task stack)
static SigData   D;
static PriceData P;
static MoneyData M;
static NetStatus N;
static Settings  S;          // what the backend uses
static Settings  E;          // local copy being edited on Billing / Settings
static uint32_t  E_dirty = 0;

// language / currency helpers (S is refreshed every second)
#define IS_EN       (S.lang == LANG_EN)
#define TR(pl, en)  (IS_EN ? (en) : (pl))
#define CUR         sig_currency(S.currency)

// energy flow lines
struct Flow {
    int ax, ay, bx, by;          // a = node centre, b = hub centre
    lv_point_precise_t pts[2];
    lv_obj_t *dot[3];
    int dir;                     // +1 node -> hub, -1 hub -> node
    int bucket;                  // 0 = stopped, 1..5 = speed
};
static Flow flows[4];
enum { F_PV = 0, F_GRID, F_BATT, F_HOME };

#define PRICE_BARS_MAX 96

// SOC history drawn over the price chart
static lv_point_precise_t soc_pts[96];
static uint8_t soc_hist[96];

// widgets of the current page (all reset on page change)
static struct Ui {
    // top bar
    lv_obj_t *status, *clock, *wifi;

    // overview: flow
    lv_obj_t *node[4], *node_val[4], *node_cap[4], *soc_arc;
    // overview: earnings
    lv_obj_t *earn_title, *earn_big, *earn_rate, *earn_split, *earn_month, *earn_yday;
    // overview: prices
    lv_obj_t *price_now, *price_exp, *price_bar[PRICE_BARS_MAX], *price_tmr, *price_chart, *price_fixed;
    lv_obj_t *soc_line, *soc_lbl;
    int       price_bars;
    // overview: tiles
    lv_obj_t *pv_big, *pv_sub, *grid_imp, *grid_exp, *grid_sub;
    lv_obj_t *bat_big, *bat_bar, *bat_sub, *home_big, *home_sub;

    // details
    lv_obj_t *row[4][8];

    // billing
    lv_obj_t *dep_val;

    // settings
    lv_obj_t *wifi_status, *wifi_list, *wifi_scan_btn;
    lv_obj_t *sig_ip, *sig_status, *inv_id;
    lv_obj_t *store_lbl;

    // export (QR) overlay
    lv_obj_t *qr_canvas, *qr_url, *qr_hint, *qr_btn[RANGE_COUNT];

    // keyboard overlay
    lv_obj_t *overlay, *ta;
    uint32_t scan_seq_shown;
} ui;

enum OverlayMode { OV_NONE, OV_WIFI_PASS, OV_IP, OV_DEPOSIT, OV_ZONE, OV_EXPORT, OV_ABOUT };
static OverlayMode ov_mode = OV_NONE;
static char ov_ssid[33];

// segmented buttons and +/- steppers (Billing / Settings)
struct Seg {
    lv_obj_t *btn[4];
    int n;
    uint8_t *target;
    bool rebuild;                // rebuild the page after change (language)
    void (*on_change)();         // optional extra action
};
static Seg segs[6];
static bool g_restarting = false;
enum { TRIAL_NONE = 0, TRIAL_CLK, TRIAL_BB };
static void trial_start(int kind, uint8_t old);
static void lcd_set_clock(uint8_t idx);
static bool g_bb_trial = false;           // booted with a new, not yet confirmed buffer mode
static int seg_count = 0;

struct Stepper {
    float *val;
    float step, lo, hi;
    int dec;
    const char *unit;            // printed after the currency, e.g. "/kWh"
    lv_obj_t *lbl;
};
static Stepper steppers[4];
static int stepper_count = 0;

static void go_page(Page p);


// ============================================================
// HELPERS
// ============================================================

static void set_text(lv_obj_t *l, const char *fmt, ...)
{
    if (!l) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, b) == 0) return;      // avoid needless redraws
    lv_label_set_text(l, b);
}

// PL: 1 234,56   EN: 1,234.56
static const char *fnum(double v, int dec)
{
    static char ring[8][24];
    static int k = 0;
    char *out = ring[k++ & 7];

    if (isnan(v) || isinf(v)) { strcpy(out, "–"); return out; }

    const char dsep = IS_EN ? '.' : ',';
    const char tsep = IS_EN ? ',' : ' ';

    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%.*f", dec, fabs(v));
    char *dot = strchr(tmp, '.');
    int int_len = dot ? (int)(dot - tmp) : (int)strlen(tmp);

    int o = 0;
    if (v < 0 && strtod(tmp, nullptr) != 0) out[o++] = '-';
    for (int i = 0; i < int_len; i++) {
        out[o++] = tmp[i];
        int left = int_len - i - 1;
        if (left > 0 && left % 3 == 0 && int_len > 4) out[o++] = tsep;
    }
    if (dot) {
        out[o++] = dsep;
        strcpy(out + o, dot + 1);
    } else {
        out[o] = 0;
    }
    return out;
}

static const char *fsigned(double v, int dec)
{
    static char ring[4][28];
    static int k = 0;
    char *out = ring[k++ & 3];
    snprintf(out, 28, "%s%s", v >= 0 ? "+" : "", fnum(v, dec));
    return out;
}

static const char *fkw(float kw)
{
    float a = fabsf(kw);
    return fnum(kw, a < 10 ? 2 : (a < 100 ? 1 : 0));
}

static const char *fkwh(double kwh)
{
    return fnum(kwh, fabs(kwh) < 100 ? 1 : 0);
}

static const char *fmoney(double v)
{
    return fnum(v, fabs(v) < 1000 ? 2 : 0);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *card(int x, int y, int w, int h)
{
    lv_obj_t *c = box(scr, x, y, w, h, C_CARD, 16);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_BORDER), 0);
    return c;
}

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t color,
                       int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *label_r(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t color,
                         int x_ofs, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, x_ofs, y);
    return l;
}

static lv_obj_t *label_dot(lv_obj_t *parent, const lv_font_t *f, uint32_t color, int x, int y, int w)
{
    lv_obj_t *l = label(parent, "", f, color, x, y);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, w, lv_font_get_line_height(f) + 1);
    return l;
}

static void set_color(lv_obj_t *l, uint32_t c)
{
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
}

static lv_obj_t *card_title(lv_obj_t *c, const char *icon, uint32_t icon_color, const char *title)
{
    label(c, icon, &font_pl_18, icon_color, 14, 10);
    return label(c, title, &font_pl_18, C_TEXT, 42, 10);
}

static lv_obj_t *button(lv_obj_t *parent, const char *t, int x, int y, int w, int h,
                        uint32_t color, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, &font_pl_18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static bool data_fresh()
{
    return D.valid && (millis() - D.updated_ms < 30000);
}

static bool local_tm(struct tm &t)
{
    time_t now = time(nullptr);
    if (now < 1700000000) return false;
    localtime_r(&now, &t);
    return true;
}


// ============================================================
// PAGE MANAGEMENT
// ============================================================

static void stop_flows()
{
    for (int i = 0; i < 4; i++) {
        lv_anim_delete(&flows[i], nullptr);
        flows[i].bucket = -1;
        for (int k = 0; k < 3; k++) flows[i].dot[k] = nullptr;
    }
}

static void clear_page()
{
    stop_flows();
    lv_obj_clean(scr);
    memset(&ui, 0, sizeof(ui));
    seg_count = 0;
    stepper_count = 0;
    ov_mode = OV_NONE;
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

static void tab_cb(lv_event_t *e)
{
    go_page((Page)(intptr_t)lv_event_get_user_data(e));
}

static void build_topbar()
{
    label(scr, ICON_SOLAR, &font_pl_24, C_PV, 14, 9);
    label(scr, "SigDash", &font_pl_18, C_TEXT, 50, 3);
    ui.status = label_dot(scr, &font_pl_14, C_DIM, 50, 25, 192);

    const char *names[PAGE_COUNT] = {
        TR("Przegląd", "Overview"), TR("Szczegóły", "Details"),
        TR("Rozliczenie", "Billing"), TR("Ustawienia", "Settings")
    };
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_set_pos(b, 250 + i * 104, 6);
        lv_obj_set_size(b, 98, 34);
        lv_obj_set_style_radius(b, 17, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == page ? C_ACCENT : C_CARD), 0);
        lv_obj_set_style_border_width(b, i == page ? 0 : 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_BORDER), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_font(l, &font_pl_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(i == page ? C_TEXT : C_DIM), 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    ui.clock = label_r(scr, "--:--", &font_pl_24, C_TEXT, -14, 8);
    ui.wifi = label_r(scr, ICON_WIFI, &font_pl_18, C_MUTED, -92, 12);
}

static void update_topbar()
{
    struct tm t;
    if (local_tm(t)) set_text(ui.clock, "%02d:%02d", t.tm_hour, t.tm_min);

    uint32_t wc = !N.wifi_ok ? C_IMPORT : (N.rssi > -60 ? C_EXPORT : (N.rssi > -75 ? C_PV : C_IMPORT));
    set_color(ui.wifi, wc);

    switch (N.sig_state) {
    case SIG_NO_WIFI:
        set_text(ui.status, "%s", N.wifi_connecting ? TR("Łączenie z Wi-Fi…", "Connecting to Wi-Fi…")
                                                    : TR("Brak Wi-Fi", "No Wi-Fi"));
        set_color(ui.status, C_IMPORT);
        break;
    case SIG_SEARCHING:
        set_text(ui.status, TR("Szukam Sigenergy… %d/254", "Searching Sigenergy… %d/254"), N.discover_progress);
        set_color(ui.status, C_PV);
        break;
    case SIG_CONNECTING:
        set_text(ui.status, TR("Łączenie z %s…", "Connecting to %s…"), N.sig_ip);
        set_color(ui.status, C_PV);
        break;
    case SIG_OK: {
        uint32_t age = (millis() - D.updated_ms) / 1000;
        if (age > 20) set_text(ui.status, TR("%s · dane sprzed %u s", "%s · data %u s old"), N.sig_ip, (unsigned)age);
        else set_text(ui.status, "%s · %s", N.sig_ip, sig_run_state_text(D.run_state, S.lang));
        set_color(ui.status, D.run_state == 2 ? C_IMPORT : C_DIM);
        break;
    }
    case SIG_NOT_FOUND:
        set_text(ui.status, "%s", TR("Nie znaleziono Sigenergy", "Sigenergy not found"));
        set_color(ui.status, C_IMPORT);
        break;
    case SIG_ERROR:
        set_text(ui.status, "%s", N.msg);
        set_color(ui.status, C_IMPORT);
        break;
    default:
        set_text(ui.status, "%s", TR("Start…", "Starting…"));
        break;
    }
}


// ============================================================
// OVERVIEW - energy flow
// ============================================================

static void flow_exec(void *var, int32_t v)
{
    Flow *f = (Flow *)var;
    for (int k = 0; k < 3; k++) {
        if (!f->dot[k]) continue;
        int32_t t = (v + k * 333) % 1000;
        if (f->dir < 0) t = 1000 - t;
        int x = f->ax + (f->bx - f->ax) * t / 1000;
        int y = f->ay + (f->by - f->ay) * t / 1000;
        lv_obj_set_pos(f->dot[k], x - 5, y - 5);
    }
}

// kw > 0 = energy flows towards the hub
static void flow_set(int i, float kw)
{
    Flow &f = flows[i];
    if (!f.dot[0]) return;

    float a = fabsf(kw);
    int bucket = (a < 0.05f) ? 0 : (1 + min(4, (int)(a / 1.5f)));
    int dir = kw >= 0 ? 1 : -1;
    if (bucket == f.bucket && (bucket == 0 || dir == f.dir)) return;

    lv_anim_delete(&f, flow_exec);
    f.bucket = bucket;
    f.dir = dir;

    for (int k = 0; k < 3; k++) {
        if (bucket == 0) lv_obj_add_flag(f.dot[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(f.dot[k], LV_OBJ_FLAG_HIDDEN);
    }
    if (bucket == 0) return;

    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, &f);
    lv_anim_set_exec_cb(&an, flow_exec);
    lv_anim_set_values(&an, 0, 999);
    lv_anim_set_duration(&an, 2800 - bucket * 420);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&an, lv_anim_path_linear);
    lv_anim_start(&an);
}

static lv_obj_t *flow_node(lv_obj_t *parent, int cx, int cy, const char *icon, uint32_t color, int idx)
{
    const int R = 44;
    lv_obj_t *n = box(parent, cx - R, cy - R, 2 * R, 2 * R, C_CARD2, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(n, 2, 0);
    lv_obj_set_style_border_color(n, lv_color_hex(color), 0);

    lv_obj_t *ic = label(n, icon, &font_pl_24, color, 0, 0);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -13);

    ui.node_val[idx] = label(n, "–", &font_pl_18, C_TEXT, 0, 0);
    lv_obj_align(ui.node_val[idx], LV_ALIGN_CENTER, 0, 15);
    ui.node[idx] = n;
    return n;
}

static void build_flow_card()
{
    lv_obj_t *c = card(12, 52, 420, 300);

    const int HX = 210, HY = 150;
    const int NX[4] = {210, 64, 356, 210};
    const int NY[4] = {54, 150, 150, 246};
    const uint32_t COL[4] = {C_PV, C_IMPORT, C_BATT, C_HOME};

    for (int i = 0; i < 4; i++) {
        Flow &f = flows[i];
        f.ax = NX[i]; f.ay = NY[i];
        f.bx = HX;    f.by = HY;
        f.pts[0].x = f.ax; f.pts[0].y = f.ay;
        f.pts[1].x = f.bx; f.pts[1].y = f.by;
        f.bucket = -1;
        f.dir = 1;

        lv_obj_t *ln = lv_line_create(c);
        lv_line_set_points(ln, f.pts, 2);
        lv_obj_set_style_line_width(ln, 3, 0);
        lv_obj_set_style_line_color(ln, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_line_rounded(ln, true, 0);

        for (int k = 0; k < 3; k++) {
            f.dot[k] = box(c, f.ax - 5, f.ay - 5, 10, 10, COL[i], LV_RADIUS_CIRCLE);
            lv_obj_add_flag(f.dot[k], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_t *hub = box(c, HX - 24, HY - 24, 48, 48, C_CARD2, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(hub, 2, 0);
    lv_obj_set_style_border_color(hub, lv_color_hex(C_ACCENT), 0);
    lv_obj_t *hi = label(hub, ICON_BOLT, &font_pl_18, C_ACCENT, 0, 0);
    lv_obj_center(hi);

    ui.soc_arc = lv_arc_create(c);
    lv_obj_set_size(ui.soc_arc, 104, 104);
    lv_obj_set_pos(ui.soc_arc, NX[F_BATT] - 52, NY[F_BATT] - 52);
    lv_arc_set_rotation(ui.soc_arc, 270);
    lv_arc_set_bg_angles(ui.soc_arc, 0, 360);
    lv_arc_set_range(ui.soc_arc, 0, 100);
    lv_arc_set_value(ui.soc_arc, 0);
    lv_obj_remove_style(ui.soc_arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(ui.soc_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ui.soc_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.soc_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.soc_arc, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.soc_arc, lv_color_hex(C_BATT), LV_PART_INDICATOR);

    flow_node(c, NX[F_PV],   NY[F_PV],   ICON_SUN,    C_PV,     F_PV);
    flow_node(c, NX[F_GRID], NY[F_GRID], ICON_TOWER,  C_IMPORT, F_GRID);
    flow_node(c, NX[F_BATT], NY[F_BATT], ICON_BATT_3, C_BATT,   F_BATT);
    flow_node(c, NX[F_HOME], NY[F_HOME], ICON_HOUSE,  C_HOME,   F_HOME);

    ui.node_cap[F_PV]   = label(c, TR("Produkcja PV", "Solar"), &font_pl_14, C_DIM, 262, 44);
    ui.node_cap[F_HOME] = label(c, TR("Zużycie domu", "Home"), &font_pl_14, C_DIM, 262, 236);
    ui.node_cap[F_GRID] = label(c, TR("Sieć", "Grid"), &font_pl_14, C_DIM, 0, 0);
    lv_obj_align(ui.node_cap[F_GRID], LV_ALIGN_TOP_LEFT, 14, 202);
    ui.node_cap[F_BATT] = label(c, TR("Bateria", "Battery"), &font_pl_14, C_DIM, 0, 0);
    lv_obj_align(ui.node_cap[F_BATT], LV_ALIGN_TOP_RIGHT, -14, 206);
}

static void update_flow()
{
    if (!ui.node_val[0]) return;
    bool ok = data_fresh();

    set_text(ui.node_val[F_PV],   ok ? "%s kW" : "–", fkw(D.pv_kw));
    set_text(ui.node_val[F_GRID], ok ? "%s kW" : "–", fkw(fabsf(D.grid_kw)));
    set_text(ui.node_val[F_BATT], ok ? "%s kW" : "–", fkw(fabsf(D.ess_kw)));
    set_text(ui.node_val[F_HOME], ok ? "%s kW" : "–", fkw(D.load_kw));

    uint32_t gc = C_MUTED;
    if (!ok) {
        set_text(ui.node_cap[F_GRID], "%s", TR("Sieć", "Grid"));
    } else if (D.grid_kw > 0.05f) {
        set_text(ui.node_cap[F_GRID], "%s", TR("Pobór z sieci", "Importing"));
        gc = C_IMPORT;
    } else if (D.grid_kw < -0.05f) {
        set_text(ui.node_cap[F_GRID], "%s", TR("Oddawanie", "Exporting"));
        gc = C_EXPORT;
    } else {
        set_text(ui.node_cap[F_GRID], "%s", TR("Sieć – bez przepływu", "Grid – idle"));
    }
    set_color(ui.node_cap[F_GRID], gc == C_MUTED ? C_DIM : gc);
    lv_obj_set_style_border_color(ui.node[F_GRID], lv_color_hex(gc == C_MUTED ? C_BORDER : gc), 0);
    for (int k = 0; k < 3; k++) lv_obj_set_style_bg_color(flows[F_GRID].dot[k], lv_color_hex(gc), 0);

    const char *bs = !ok ? TR("Bateria", "Battery")
                   : (D.ess_kw > 0.05f ? TR("Ładowanie", "Charging")
                   : (D.ess_kw < -0.05f ? TR("Rozładowanie", "Discharging") : TR("Bateria", "Battery")));
    set_text(ui.node_cap[F_BATT], ok ? "%s · %s%%" : "%s", bs, fnum(D.soc, 0));
    lv_arc_set_value(ui.soc_arc, ok ? (int)(D.soc + 0.5f) : 0);

    flow_set(F_PV,   ok ? D.pv_kw : 0);
    flow_set(F_GRID, ok ? D.grid_kw : 0);
    flow_set(F_BATT, ok ? -D.ess_kw : 0);
    flow_set(F_HOME, ok ? -D.load_kw : 0);
}


// ============================================================
// OVERVIEW - earnings / prosumer account
// ============================================================

static void build_earn_card()
{
    lv_obj_t *c = card(444, 52, 344, 150);
    bool dep = S.earn_mode == EARN_DEPOSIT;
    ui.earn_title = card_title(c, dep ? ICON_PIGGY : ICON_COINS, C_GOLD,
                               dep ? TR("Konto prosumenta", "Prosumer account")
                                   : TR("Zarobek dziś", "Earned today"));
    if (!dep) ui.earn_yday = label_r(c, "", &font_pl_14, C_DIM, -14, 14);

    ui.earn_big = label(c, "", &font_num_48, C_GOLD, 14, 34);

    ui.earn_rate = label_r(c, "", &font_pl_18, C_EXPORT, -14, 52);
    label_r(c, TR("teraz", "now"), &font_pl_14, C_DIM, -14, 74);

    ui.earn_split = label_dot(c, &font_pl_14, C_DIM, 14, 96, 316);
    ui.earn_month = label_dot(c, &font_pl_14, C_DIM, 14, 120, 316);
}

static void update_earn_card()
{
    if (!ui.earn_big) return;

    if (S.earn_mode == EARN_DEPOSIT) {
        set_text(ui.earn_big, "%s %s", fmoney(M.deposit), CUR);
        set_text(ui.earn_rate, "%s %s/h", fsigned(M.rate_dep_h, 2), CUR);
        set_color(ui.earn_rate, M.rate_dep_h >= 0 ? C_EXPORT : C_IMPORT);
        set_text(ui.earn_split, TR("Dziś: wpływ +%s  ·  pobór −%s %s", "Today: in +%s  ·  used −%s %s"),
                 fmoney(M.today.dep_in_pln), fmoney(M.today.dep_out_pln), CUR);
        set_text(ui.earn_month, TR("Wczoraj %s  ·  miesiąc netto %s %s", "Yesterday %s  ·  month net %s %s"),
                 fsigned(M.yesterday_dep, 2), fsigned(M.month.dep_in_pln - M.month.dep_out_pln, 2), CUR);
    } else {
        double e = M.today.saved_pln + M.today.export_pln;
        set_text(ui.earn_big, "%s %s", fmoney(e), CUR);
        set_text(ui.earn_rate, "+%s %s/h", fnum(M.rate_pln_h, 2), CUR);
        set_color(ui.earn_rate, C_EXPORT);
        set_text(ui.earn_yday, TR("wczoraj %s %s", "yesterday %s %s"), fmoney(M.yesterday_earn), CUR);
        set_text(ui.earn_split, TR("Autokonsumpcja %s %s  ·  Sprzedaż %s %s", "Self-use %s %s  ·  Export %s %s"),
                 fmoney(M.today.saved_pln), CUR, fmoney(M.today.export_pln), CUR);
        set_text(ui.earn_month, TR("Miesiąc %s %s  ·  Łącznie %s %s", "Month %s %s  ·  Total %s %s"),
                 fmoney(M.month.saved_pln + M.month.export_pln), CUR,
                 fmoney(M.total.saved_pln + M.total.export_pln), CUR);
    }
}


// ============================================================
// OVERVIEW - prices
// ============================================================

static void build_price_card()
{
    lv_obj_t *c = card(444, 210, 344, 142);
    bool fixed = S.price_src == SRC_FIXED;
    char title[48];
    const char *res = S.price_res == RES_QUARTER ? "15 min" : "1 h";
    if (fixed) snprintf(title, sizeof(title), "%s", TR("Cena sprzedaży", "Export price"));
    else if (S.price_src == SRC_EC) snprintf(title, sizeof(title), "%s · %s", SIG_ZONES[S.zone].code, res);
    else snprintf(title, sizeof(title), "RCE · %s", res);
    lv_obj_t *tl = card_title(c, ICON_CHART, C_ACCENT, title);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(tl, 160, 22);
    ui.price_now = label_r(c, "–", &font_pl_18, C_TEXT, -14, 10);
    ui.price_exp = label_dot(c, &font_pl_14, C_DIM, 14, 34, 316);

    if (fixed) {
        ui.price_fixed = label(c, "", &font_pl_14, C_DIM, 14, 64);
        lv_label_set_long_mode(ui.price_fixed, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ui.price_fixed, 316);
        ui.price_bars = 0;
        return;
    }

    // bar chart: 24 hourly bars or 96 quarter bars
    const int X0 = 16, Y0 = 56, H = 50, WIDTH = 312;
    ui.price_chart = box(c, 0, 0, 344, 142, 0, 0);
    lv_obj_set_style_bg_opa(ui.price_chart, LV_OPA_TRANSP, 0);
    box(ui.price_chart, X0, Y0 + H, WIDTH, 1, C_BORDER, 0);

    ui.price_bars = (S.price_res == RES_QUARTER) ? 96 : 24;
    int step = WIDTH / ui.price_bars;            // 13 px or 3 px
    int w = (ui.price_bars == 24) ? step - 2 : step;
    for (int i = 0; i < ui.price_bars; i++) {
        ui.price_bar[i] = box(ui.price_chart, X0 + i * step, Y0 + H - 2, w, 2, C_MUTED,
                              ui.price_bars == 24 ? 3 : 0);
    }
    const char *hl[5] = {"0", "6", "12", "18", "24"};
    for (int i = 0; i < 5; i++) {
        int x = X0 + i * WIDTH / 4 - (i == 4 ? 12 : 0);
        label(ui.price_chart, hl[i], &font_pl_14, C_MUTED, x, Y0 + H + 2);
    }

    // battery SOC (0..100 % over the chart height) - today's history
    ui.soc_line = lv_line_create(ui.price_chart);
    lv_obj_set_style_line_width(ui.soc_line, 3, 0);
    lv_obj_set_style_line_color(ui.soc_line, lv_color_hex(C_BATT), 0);
    lv_obj_set_style_line_rounded(ui.soc_line, true, 0);
    lv_obj_add_flag(ui.soc_line, LV_OBJ_FLAG_HIDDEN);
    ui.soc_lbl = label(ui.price_chart, "", &font_pl_14, C_BATT, 0, 0);
    lv_obj_add_flag(ui.soc_lbl, LV_OBJ_FLAG_HIDDEN);
    ui.price_tmr = label_dot(c, &font_pl_14, C_DIM, 14, 122, 316);
}

static void update_soc_line()
{
    if (!ui.soc_line) return;
    sig_get_soc_history(soc_hist);

    const int X0 = 16, Y0 = 56, H = 50, WIDTH = 312;
    int n = 0, last = -1;
    for (int i = 0; i < 96; i++) {
        if (soc_hist[i] > 100) continue;          // no data in this quarter (gaps are bridged)
        soc_pts[n].x = X0 + (i * WIDTH + WIDTH / 2) / 96;
        soc_pts[n].y = Y0 + H - soc_hist[i] * H / 100;
        n++;
        last = i;
    }
    if (n < 2) {
        lv_obj_add_flag(ui.soc_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.soc_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_line_set_points(ui.soc_line, soc_pts, n);
    lv_obj_remove_flag(ui.soc_line, LV_OBJ_FLAG_HIDDEN);

    // "64%" next to the newest point
    set_text(ui.soc_lbl, "%d%%", soc_hist[last]);
    int lx = (int)soc_pts[n - 1].x + 5;
    int ly = (int)soc_pts[n - 1].y - 9;
    if (lx > X0 + WIDTH - 30) lx = (int)soc_pts[n - 1].x - 36;
    ly = constrain(ly, Y0 - 16, Y0 + H - 14);
    lv_obj_set_pos(ui.soc_lbl, lx, ly);
    lv_obj_remove_flag(ui.soc_lbl, LV_OBJ_FLAG_HIDDEN);
}

static uint32_t price_color(float v, float lo, float hi)
{
    if (isnan(v)) return C_MUTED;
    if (v < 0) return C_BATT;
    float t = (hi > lo) ? (v - lo) / (hi - lo) : 0.5f;
    if (t < 0.33f) return C_EXPORT;
    if (t < 0.66f) return C_PV;
    return C_IMPORT;
}

static void update_price_card()
{
    if (!ui.price_now) return;
    bool fixed = S.price_src == SRC_FIXED;

    if (M.price_now_ok) {
        set_text(ui.price_now, "%s %s/kWh", fnum(M.price_now_pln_kwh, 2), CUR);
        if (fixed)
            set_text(ui.price_exp, TR("Zakup %s %s/kWh  ·  energia %s %s/kWh", "Import %s %s/kWh  ·  energy %s %s/kWh"),
                     fnum(S.import_price, 2), CUR, fnum(S.energy_price, 2), CUR);
        else if (S.price_src == SRC_RCE && S.export_coef > 1.001f)
            set_text(ui.price_exp, TR("Sprzedaż ×%s = %s  ·  zakup %s %s/kWh", "Export ×%s = %s  ·  import %s %s/kWh"),
                     fnum(S.export_coef, 2), fnum(M.export_price_now, 2), fnum(S.import_price, 2), CUR);
        else
            set_text(ui.price_exp, TR("Sprzedaż %s  ·  zakup %s %s/kWh", "Export %s  ·  import %s %s/kWh"),
                     fnum(M.export_price_now, 2), fnum(S.import_price, 2), CUR);
    } else {
        set_text(ui.price_now, "%s", TR("brak danych", "no data"));
        if (S.price_src == SRC_EC)
            set_text(ui.price_exp, "%s", TR("Pobieranie cen z Energy-Charts…", "Downloading prices from Energy-Charts…"));
        else
            set_text(ui.price_exp, "%s", TR("Pobieranie cen z PSE…", "Downloading prices from PSE…"));
    }

    if (fixed) {
        set_text(ui.price_fixed, "%s",
                 TR("Stała stawka sprzedaży – zmień w zakładce Rozliczenie.",
                    "Fixed export price – change it on the Billing tab."));
        return;
    }

    const float *src = (ui.price_bars == 96) ? P.q_today : P.today;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < ui.price_bars; i++) {
        float v = src[i];
        if (!P.ok_today || isnan(v)) continue;
        lo = min(lo, v);
        hi = max(hi, v);
    }
    float top = max(hi, 50.0f);

    struct tm t;
    int now_i = -1;
    if (local_tm(t)) now_i = (ui.price_bars == 96) ? t.tm_hour * 4 + t.tm_min / 15 : t.tm_hour;

    for (int i = 0; i < ui.price_bars; i++) {
        float v = P.ok_today ? src[i] : NAN;
        int bh = 2;
        if (!isnan(v) && v > 0) bh = max(2, (int)(v / top * 50.0f));
        lv_obj_t *b = ui.price_bar[i];
        lv_obj_set_height(b, bh);
        lv_obj_set_y(b, 56 + 50 - bh);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == now_i ? C_TEXT : price_color(v, lo, hi)), 0);
        lv_obj_set_style_bg_opa(b, i == now_i ? LV_OPA_COVER : LV_OPA_70, 0);
    }

    update_soc_line();

    if (P.ok_tomorrow) {
        float tl = 1e9f, th = -1e9f, ts = 0;
        int n = 0;
        for (int h = 0; h < 24; h++) {
            float v = P.tomorrow[h];
            if (isnan(v)) continue;
            tl = min(tl, v); th = max(th, v); ts += v; n++;
        }
        if (n) set_text(ui.price_tmr, TR("Jutro: min %s · śr. %s · max %s %s", "Tomorrow: min %s · avg %s · max %s %s"),
                        fnum(tl / 1000, 2), fnum(ts / n / 1000, 2), fnum(th / 1000, 2), CUR);
    } else if (P.ok_today) {
        set_text(ui.price_tmr, TR("Dziś %s – %s %s  ·  jutro po 14:00", "Today %s – %s %s  ·  tomorrow after 14:00"),
                 fnum(lo / 1000, 2), fnum(hi / 1000, 2), CUR);
    } else {
        set_text(ui.price_tmr, "");
    }
}


// ============================================================
// OVERVIEW - tiles
// ============================================================

static lv_obj_t *tile(int i, const char *icon, uint32_t icolor, const char *title)
{
    lv_obj_t *t = card(12 + i * 196, 362, 188, 110);
    label(t, icon, &font_pl_14, icolor, 12, 10);
    label(t, title, &font_pl_14, C_DIM, 34, 10);
    return t;
}

static void build_tiles()
{
    lv_obj_t *t;

    t = tile(0, ICON_SUN, C_PV, TR("Produkcja dziś", "Solar today"));
    ui.pv_big = label(t, "–", &font_pl_32, C_TEXT, 12, 32);
    ui.pv_sub = label(t, "", &font_pl_14, C_DIM, 12, 80);

    t = tile(1, ICON_TOWER, C_IMPORT, TR("Sieć dziś", "Grid today"));
    label(t, ICON_ARROW_DOWN, &font_pl_18, C_IMPORT, 12, 34);
    ui.grid_imp = label(t, "–", &font_pl_18, C_TEXT, 36, 34);
    label(t, ICON_ARROW_UP, &font_pl_18, C_EXPORT, 12, 58);
    ui.grid_exp = label(t, "–", &font_pl_18, C_TEXT, 36, 58);
    ui.grid_sub = label(t, "", &font_pl_14, C_DIM, 12, 84);

    t = tile(2, ICON_BATT_3, C_BATT, TR("Bateria", "Battery"));
    ui.bat_big = label(t, "–", &font_pl_32, C_TEXT, 12, 32);
    ui.bat_bar = lv_bar_create(t);
    lv_obj_set_size(ui.bat_bar, 70, 14);
    lv_obj_set_pos(ui.bat_bar, 106, 46);
    lv_bar_set_range(ui.bat_bar, 0, 100);
    lv_obj_set_style_bg_color(ui.bat_bar, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.bat_bar, lv_color_hex(C_BATT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.bat_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.bat_bar, 7, LV_PART_INDICATOR);
    ui.bat_sub = label(t, "", &font_pl_14, C_DIM, 12, 80);

    t = tile(3, ICON_HOUSE, C_HOME, TR("Zużycie dziś", "Home today"));
    ui.home_big = label(t, "–", &font_pl_32, C_TEXT, 12, 32);
    ui.home_sub = label(t, "", &font_pl_14, C_DIM, 12, 80);
}

static void update_tiles()
{
    if (!ui.pv_big) return;
    bool ok = data_fresh();

    double pv_today = D.have_pv_daily ? D.pv_today_reg : M.today.kwh_pv;
    set_text(ui.pv_big, "%s kWh", fkwh(pv_today));
    if (D.have_pv_daily) set_text(ui.pv_sub, TR("wczoraj %s kWh", "yesterday %s kWh"), fkwh(D.pv_yday_reg));
    else set_text(ui.pv_sub, "%s", TR("od północy (licznik)", "since midnight (meter)"));

    set_text(ui.grid_imp, "%s kWh", fkwh(M.today.kwh_imp));
    set_text(ui.grid_exp, "%s kWh", fkwh(M.today.kwh_exp));
    double bil = M.today.kwh_exp - M.today.kwh_imp;
    set_text(ui.grid_sub, TR("Bilans %s kWh", "Balance %s kWh"), fsigned(bil, fabs(bil) < 100 ? 1 : 0));

    set_text(ui.bat_big, ok ? "%s%%" : "–", fnum(D.soc, 0));
    lv_bar_set_value(ui.bat_bar, ok ? (int)(D.soc + 0.5f) : 0, LV_ANIM_ON);
    float temp = D.inv_ok ? D.bat_temp_avg : D.plant_cell_temp;
    set_text(ui.bat_sub, "%s°C  ·  SOH %s%%", fnum(temp, 1), fnum(D.soh, 0));

    set_text(ui.home_big, "%s kWh", fkwh(M.today.kwh_load));
    double aut = M.today.kwh_load > 0.05 ? (1.0 - M.today.kwh_imp / M.today.kwh_load) * 100.0 : 0;
    if (aut < 0) aut = 0;
    set_text(ui.home_sub, TR("Autarkia %s%%", "Self-sufficiency %s%%"), fnum(aut, 0));
}

static void build_overview()
{
    build_flow_card();
    build_earn_card();
    build_price_card();
    build_tiles();
}


// ============================================================
// DETAILS
// ============================================================

static const char *DETAIL_LABELS[2][4][8] = {
    {   // PL
        {"Moc L1 / L2 / L3", "Napięcie L1 / L2 / L3", "Prąd L1 / L2 / L3", "Częstotliwość",
         "Współczynnik mocy", "Praca", "Licznik sieci", "Łącznie pobór / oddanie"},
        {"Moc teraz", "PV1", "PV2", "PV3", "PV4", "Dziś / wczoraj", "Rezystancja izolacji", "Łącznie"},
        {"Poziom / kondycja", "Moc", "Energia dostępna", "Temp. śr / min / max",
         "Ogniwa min / max", "Dziś ładow. / rozład.", "Maks. ładow. / rozład.", "Limity SOC"},
        {"Stan instalacji", "Tryb EMS", "Falownik", "Moc falownika",
         "Alarmy", "Sigenergy", "Ostatni odczyt", "Ceny"}
    },
    {   // EN
        {"Power L1 / L2 / L3", "Voltage L1 / L2 / L3", "Current L1 / L2 / L3", "Frequency",
         "Power factor", "Grid mode", "Grid meter", "Total import / export"},
        {"Power now", "PV1", "PV2", "PV3", "PV4", "Today / yesterday", "Insulation resistance", "Total"},
        {"Charge / health", "Power", "Energy available", "Temp. avg / min / max",
         "Cells min / max", "Today charge / disch.", "Max charge / disch.", "SOC limits"},
        {"Plant state", "EMS mode", "Inverter", "Inverter power",
         "Alarms", "Sigenergy", "Last read", "Prices"}
    }
};

static void build_details()
{
    const int X[4] = {12, 406, 12, 406};
    const int Y[4] = {52, 52, 266, 266};
    const char *ICON[4]  = {ICON_TOWER, ICON_SOLAR, ICON_BATT_3, ICON_MICROCHIP};
    const uint32_t IC[4] = {C_IMPORT, C_PV, C_BATT, C_ACCENT};
    const char *TITLE[4] = {TR("Sieć", "Grid"), TR("Fotowoltaika", "Solar"),
                            TR("Bateria", "Battery"), TR("System", "System")};
    int L = IS_EN ? 1 : 0;

    for (int c = 0; c < 4; c++) {
        lv_obj_t *k = card(X[c], Y[c], 382, 206);
        card_title(k, ICON[c], IC[c], TITLE[c]);
        for (int r = 0; r < 8; r++) {
            label(k, DETAIL_LABELS[L][c][r], &font_pl_14, C_DIM, 14, 40 + r * 20);
            ui.row[c][r] = label_r(k, "–", &font_pl_14, C_TEXT, -14, 40 + r * 20);
        }
    }
}

static void update_details()
{
    if (!ui.row[0][0]) return;
    lv_obj_t *(*R)[8] = ui.row;
    const char *na = "–";

    // --- grid ---
    set_text(R[0][0], "%s / %s / %s kW", fkw(D.grid_ph_kw[0]), fkw(D.grid_ph_kw[1]), fkw(D.grid_ph_kw[2]));
    if (D.inv_ok) {
        set_text(R[0][1], "%s / %s / %s V", fnum(D.v_ph[0], 0), fnum(D.v_ph[1], 0), fnum(D.v_ph[2], 0));
        set_text(R[0][2], "%s / %s / %s A", fnum(D.i_ph[0], 1), fnum(D.i_ph[1], 1), fnum(D.i_ph[2], 1));
        set_text(R[0][3], "%s Hz", fnum(D.grid_freq, 2));
        set_text(R[0][4], "%s", fnum(D.pf, 3));
    } else {
        for (int i = 1; i <= 4; i++) set_text(R[0][i], "%s", na);
    }
    set_text(R[0][5], "%s", D.on_off_grid == 0 ? TR("Na sieci", "On grid")
                           : (D.on_off_grid == 1 ? TR("Wyspa (auto)", "Off grid (auto)")
                                                 : TR("Wyspa (ręcznie)", "Off grid (manual)")));
    set_text(R[0][6], "%s", D.grid_sensor_ok ? TR("Połączony", "Connected") : TR("Brak", "Missing"));
    if (D.have_acc_grid) set_text(R[0][7], "%s / %s kWh", fnum(D.acc_imp, 0), fnum(D.acc_exp, 0));
    else set_text(R[0][7], "%s", na);

    // --- PV ---
    set_text(R[1][0], "%s kW", fkw(D.pv_kw));
    for (int i = 0; i < 4; i++) {
        if (D.inv_ok && D.pv_v[i] > 5)
            set_text(R[1][1 + i], "%s V · %s A · %s kW", fnum(D.pv_v[i], 0), fnum(D.pv_i[i], 2),
                     fkw(D.pv_v[i] * D.pv_i[i] / 1000.0f));
        else
            set_text(R[1][1 + i], "%s", na);
    }
    if (D.have_pv_daily) set_text(R[1][5], "%s / %s kWh", fkwh(D.pv_today_reg), fkwh(D.pv_yday_reg));
    else set_text(R[1][5], TR("%s kWh (licznik)", "%s kWh (meter)"), fkwh(M.today.kwh_pv));
    set_text(R[1][6], D.inv_ok ? "%s MOhm" : "%s", D.inv_ok ? fnum(D.insulation_mohm, 2) : na);
    set_text(R[1][7], D.have_acc_pv ? "%s kWh" : "%s", D.have_acc_pv ? fnum(D.acc_pv, 0) : na);

    // --- battery ---
    set_text(R[2][0], "%s%% / %s%%", fnum(D.soc, 1), fnum(D.soh, 0));
    const char *mode = D.ess_kw > 0.05f ? TR("Ładowanie", "Charging")
                     : (D.ess_kw < -0.05f ? TR("Rozładowanie", "Discharging") : TR("Spoczynek", "Idle"));
    set_text(R[2][1], "%s %s kW", mode, fkw(fabsf(D.ess_kw)));
    set_text(R[2][2], TR("%s z %s kWh", "%s of %s kWh"), fkwh(D.ess_avail_dis_kwh), fkwh(D.ess_rated_kwh));
    if (D.inv_ok) {
        set_text(R[2][3], "%s / %s / %s°C", fnum(D.bat_temp_avg, 1), fnum(D.bat_temp_min, 1), fnum(D.bat_temp_max, 1));
        set_text(R[2][4], "%s / %s V", fnum(D.cell_v_min, 3), fnum(D.cell_v_max, 3));
        set_text(R[2][5], "%s / %s kWh", fkwh(D.ess_day_chg_kwh), fkwh(D.ess_day_dis_kwh));
    } else {
        set_text(R[2][3], "%s°C", fnum(D.plant_cell_temp, 1));
        set_text(R[2][4], "%s", na);
        set_text(R[2][5], "%s / %s kWh", fkwh(M.today.kwh_bchg), fkwh(M.today.kwh_bdis));
    }
    set_text(R[2][6], "%s / %s kW", fkw(D.ess_max_chg_kw), fkw(D.ess_max_dis_kw));
    set_text(R[2][7], "%s%% – %s%%", fnum(D.dis_cutoff_soc, 0), fnum(D.chg_cutoff_soc, 0));

    // --- system ---
    set_text(R[3][0], "%s", sig_run_state_text(D.run_state, S.lang));
    set_color(R[3][0], D.run_state == 2 ? C_IMPORT : C_TEXT);
    set_text(R[3][1], "%s", sig_ems_mode_text(D.ems_mode, S.lang));
    if (D.inv_ok) set_text(R[3][2], "%s · %s°C", sig_run_state_text(D.inv_state, S.lang), fnum(D.inv_temp, 1));
    else set_text(R[3][2], TR("brak odczytu (ID %d)", "no data (ID %d)"), S.inv_id);
    set_text(R[3][3], D.inv_ok ? "%s kW" : "%s", D.inv_ok ? fkw(D.inv_kw) : na);

    int alarm_idx = -1;
    for (int i = 0; i < 7; i++) if (D.alarms[i]) { alarm_idx = i; break; }
    int inv_alarm_idx = -1;
    for (int i = 0; i < 5; i++) if (D.inv_alarms[i]) { inv_alarm_idx = i; break; }
    if (alarm_idx >= 0) {
        set_text(R[3][4], TR("Instalacja %d: 0x%04X", "Plant %d: 0x%04X"), alarm_idx + 1, D.alarms[alarm_idx]);
        set_color(R[3][4], C_IMPORT);
    } else if (inv_alarm_idx >= 0) {
        set_text(R[3][4], TR("Falownik %d: 0x%04X", "Inverter %d: 0x%04X"), inv_alarm_idx + 1, D.inv_alarms[inv_alarm_idx]);
        set_color(R[3][4], C_IMPORT);
    } else {
        set_text(R[3][4], "%s", TR("Brak", "None"));
        set_color(R[3][4], C_EXPORT);
    }
    set_text(R[3][5], "%s (ID %d)", N.sig_ip[0] ? N.sig_ip : "?", S.inv_id);
    if (D.valid) set_text(R[3][6], TR("%u s temu · #%u", "%u s ago · #%u"),
                          (unsigned)((millis() - D.updated_ms) / 1000), (unsigned)D.poll_count);
    else set_text(R[3][6], "%s", na);
    if (S.price_src == SRC_FIXED) set_text(R[3][7], "%s", TR("stała stawka", "fixed price"));
    else if (P.ok_today) set_text(R[3][7], "%s · %d/96%s",
                                  S.price_src == SRC_EC ? SIG_ZONES[S.zone].code : "PSE RCE",
                                  P.quarters_today, P.ok_tomorrow ? TR(" · +jutro", " · +tomorrow") : "");
    else set_text(R[3][7], "%s", TR("brak", "none"));
}


// ============================================================
// SEGMENTED BUTTONS + STEPPERS (edit E, saved with a delay)
// ============================================================

static void seg_restyle(Seg &s)
{
    for (int i = 0; i < s.n; i++) {
        bool on = *s.target == i;
        lv_obj_set_style_bg_color(s.btn[i], lv_color_hex(on ? C_ACCENT : C_CARD2), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s.btn[i], 0), lv_color_hex(on ? C_TEXT : C_DIM), 0);
    }
}

static void show_page(Page p);

static void seg_cb(lv_event_t *e)
{
    int ud = (int)(intptr_t)lv_event_get_user_data(e);
    Seg &s = segs[ud >> 4];
    int i = ud & 15;
    if (*s.target == i) return;
    *s.target = (uint8_t)i;
    seg_restyle(s);
    E_dirty = 1;                               // save on next update (almost immediately)
    if (s.on_change) { s.on_change(); return; }
    if (s.rebuild) {
        sig_req_update_prefs(E);
        E_dirty = 0;
        S = E;                                 // apply right away (language, source, currency)
        go_page(page);
    }
}

static void make_seg(lv_obj_t *parent, int x, int y, int w, int h,
                     const char *const *labels, int n, uint8_t *target, bool rebuild = false,
                     void (*on_change)() = nullptr)
{
    if (seg_count >= (int)(sizeof(segs) / sizeof(segs[0]))) return;
    Seg &s = segs[seg_count];
    s.on_change = on_change;
    s.n = n;
    s.target = target;
    s.rebuild = rebuild;
    int bw = (w - (n - 1) * 6) / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_button_create(parent);
        lv_obj_set_pos(b, x + i * (bw + 6), y);
        lv_obj_set_size(b, bw, h);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 2, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_font(l, &font_pl_14, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)((seg_count << 4) | i));
        s.btn[i] = b;
    }
    seg_restyle(s);
    seg_count++;
}

static void stepper_show(Stepper &st)
{
    set_text(st.lbl, "%s %s%s", fnum(*st.val, st.dec), CUR, st.unit);
}

static void stepper_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    int ud = (int)(intptr_t)lv_event_get_user_data(e);
    Stepper &st = steppers[ud >> 1];
    float v = *st.val + ((ud & 1) ? st.step : -st.step);
    *st.val = constrain(v, st.lo, st.hi);
    stepper_show(st);
    E_dirty = millis();                        // debounced save
}

static void make_stepper(lv_obj_t *parent, int y, float *val, float step, float lo, float hi,
                         int dec, const char *unit)
{
    if (stepper_count >= (int)(sizeof(steppers) / sizeof(steppers[0]))) return;
    int idx = stepper_count++;
    Stepper &st = steppers[idx];
    st.val = val; st.step = step; st.lo = lo; st.hi = hi; st.dec = dec; st.unit = unit;

    lv_obj_t *m = button(parent, "−", 14, y, 56, 40, C_CARD2, nullptr, nullptr);
    lv_obj_add_event_cb(m, stepper_cb, LV_EVENT_ALL, (void *)(intptr_t)(idx << 1));
    st.lbl = label(parent, "", &font_pl_24, C_TEXT, 0, 0);
    lv_obj_align(st.lbl, LV_ALIGN_TOP_MID, 0, y + 6);
    lv_obj_t *p = button(parent, "+", 312, y, 56, 40, C_CARD2, nullptr, nullptr);
    lv_obj_add_event_cb(p, stepper_cb, LV_EVENT_ALL, (void *)(intptr_t)((idx << 1) | 1));
    stepper_show(st);
}

static void flush_edits(bool force)
{
    if (!E_dirty) return;
    if (!force && E_dirty != 1 && millis() - E_dirty < 1200) return;
    E_dirty = 0;
    sig_req_update_prefs(E);
}

static lv_obj_t *make_switch(lv_obj_t *parent, int x, int y, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 56, 30);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

static lv_obj_t *note(lv_obj_t *parent, const char *t, int y)
{
    lv_obj_t *l = label(parent, t, &font_pl_14, C_MUTED, 14, y);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 354);
    return l;
}


// ============================================================
// KEYBOARD OVERLAY (WiFi password, IP, deposit)
// ============================================================

static void overlay_close()
{
    if (ui.overlay) lv_obj_delete(ui.overlay);
    ui.overlay = nullptr;
    ui.ta = nullptr;
    ov_mode = OV_NONE;
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *txt = lv_textarea_get_text(ui.ta);
        if (ov_mode == OV_WIFI_PASS) {
            sig_req_wifi_connect(ov_ssid, txt);
        } else if (ov_mode == OV_IP) {
            IPAddress ip;
            if (ip.fromString(txt)) sig_req_set_ip(txt);
        } else if (ov_mode == OV_DEPOSIT) {
            char b[24];
            strlcpy(b, txt, sizeof(b));
            for (char *c = b; *c; c++) if (*c == ',') *c = '.';
            if (b[0]) sig_req_set_deposit(strtod(b, nullptr));
        }
        lv_async_call([](void *) { overlay_close(); }, nullptr);
    } else if (code == LV_EVENT_CANCEL) {
        lv_async_call([](void *) { overlay_close(); }, nullptr);
    }
}

static void overlay_open(OverlayMode mode, const char *title, const char *initial)
{
    overlay_close();
    ov_mode = mode;

    ui.overlay = box(scr, 0, 0, 800, 480, C_BG, 0);
    lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_CLICKABLE);
    label(ui.overlay, title, &font_pl_24, C_TEXT, 30, 22);

    ui.ta = lv_textarea_create(ui.overlay);
    lv_obj_set_pos(ui.ta, 30, 70);
    lv_obj_set_size(ui.ta, 740, 56);
    lv_textarea_set_one_line(ui.ta, true);
    lv_obj_set_style_text_font(ui.ta, &font_pl_24, 0);
    lv_obj_set_style_bg_color(ui.ta, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_text_color(ui.ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(ui.ta, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(ui.ta, 2, 0);
    lv_obj_set_style_radius(ui.ta, 12, 0);
    lv_textarea_set_text(ui.ta, initial ? initial : "");
    if (mode == OV_WIFI_PASS) {
        lv_textarea_set_placeholder_text(ui.ta, TR("hasło Wi-Fi", "Wi-Fi password"));
    } else if (mode == OV_IP) {
        lv_textarea_set_accepted_chars(ui.ta, "0123456789.");
        lv_textarea_set_max_length(ui.ta, 15);
        lv_textarea_set_placeholder_text(ui.ta, TR("np. 192.168.1.50", "e.g. 192.168.1.50"));
    } else {
        lv_textarea_set_accepted_chars(ui.ta, "0123456789.,");
        lv_textarea_set_max_length(ui.ta, 12);
        lv_textarea_set_placeholder_text(ui.ta, TR("np. 1234,56", "e.g. 1234.56"));
    }

    lv_obj_t *kb = lv_keyboard_create(ui.overlay);
    lv_obj_set_size(kb, 800, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ui.ta);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD2), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_BORDER), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(kb, &font_pl_18, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    if (mode != OV_WIFI_PASS) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, nullptr);

    char hint[64];
    snprintf(hint, sizeof(hint), "%s " ICON_CHECK "     %s " LV_SYMBOL_KEYBOARD,
             TR("Zatwierdź:", "Confirm:"), TR("Anuluj:", "Cancel:"));
    label(ui.overlay, hint, &font_pl_14, C_DIM, 30, 136);
}


// ============================================================
// EXPORT TO EXCEL (QR code with the download link)
// ============================================================

#define QR_PX 264
static uint8_t *qr_buf = nullptr;
static int qr_range = RANGE_MONTH;

static void qr_draw(const char *text)
{
    if (!ui.qr_canvas || !qr_buf) return;
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    uint32_t stride = lv_draw_buf_width_to_stride(QR_PX, LV_COLOR_FORMAT_RGB565);

    // white background
    for (int y = 0; y < QR_PX; y++) {
        uint16_t *row = (uint16_t *)(qr_buf + y * stride);
        for (int x = 0; x < QR_PX; x++) row[x] = 0xFFFF;
    }
    if (text && qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 8, qrcodegen_Mask_AUTO, true)) {
        int n = qrcodegen_getSize(qr);
        int border = 2;
        int px = QR_PX / (n + 2 * border);
        int off = (QR_PX - px * n) / 2;
        for (int my = 0; my < n; my++) {
            for (int mx = 0; mx < n; mx++) {
                if (!qrcodegen_getModule(qr, mx, my)) continue;
                for (int y = 0; y < px; y++) {
                    uint16_t *row = (uint16_t *)(qr_buf + (off + my * px + y) * stride);
                    for (int x = 0; x < px; x++) row[off + mx * px + x] = 0x0000;
                }
            }
        }
    }
    lv_obj_invalidate(ui.qr_canvas);
}

static void export_refresh()
{
    if (ov_mode != OV_EXPORT) return;
    for (int i = 0; i < RANGE_COUNT; i++) {
        if (!ui.qr_btn[i]) continue;
        lv_obj_set_style_bg_color(ui.qr_btn[i], lv_color_hex(i == qr_range ? C_ACCENT : C_CARD2), 0);
    }
    char url[64];
    if (sig_export_url(url, sizeof(url), qr_range)) {
        qr_draw(url);
        set_text(ui.qr_url, "%s", url);
        lv_obj_remove_flag(ui.qr_canvas, LV_OBJ_FLAG_HIDDEN);
        set_text(ui.qr_hint, "%s", TR("Zeskanuj telefonem podłączonym do tego samego Wi-Fi – plik Excela pobierze się sam. "
                                      "Na komputerze wpisz adres w przeglądarce.",
                                      "Scan with a phone on the same Wi-Fi – the Excel file downloads automatically. "
                                      "On a computer, type the address into a browser."));
    } else {
        lv_obj_add_flag(ui.qr_canvas, LV_OBJ_FLAG_HIDDEN);
        set_text(ui.qr_url, "%s", TR("Brak Wi-Fi", "No Wi-Fi"));
        set_text(ui.qr_hint, "%s", TR("Eksport działa w sieci domowej – najpierw połącz się z Wi-Fi.",
                                      "Export works in the home network – connect to Wi-Fi first."));
    }
}

static void export_range_cb(lv_event_t *e)
{
    qr_range = (int)(intptr_t)lv_event_get_user_data(e);
    export_refresh();
}

static void export_open()
{
    overlay_close();
    ov_mode = OV_EXPORT;

    if (!qr_buf) {
        size_t sz = lv_draw_buf_width_to_stride(QR_PX, LV_COLOR_FORMAT_RGB565) * QR_PX;
#ifdef ESP_PLATFORM
        qr_buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        qr_buf = (uint8_t *)malloc(sz);
#endif
    }

    ui.overlay = box(scr, 0, 0, 800, 480, C_BG, 0);
    lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_CLICKABLE);
    label(ui.overlay, ICON_CHART, &font_pl_24, C_EXPORT, 20, 18);
    label(ui.overlay, TR("Eksport do Excela", "Export to Excel"), &font_pl_24, C_TEXT, 56, 16);
    button(ui.overlay, LV_SYMBOL_CLOSE, 724, 10, 60, 44, C_CARD2,
           [](lv_event_t *) { lv_async_call([](void *) { overlay_close(); }, nullptr); }, nullptr);

    const char *pl[RANGE_COUNT] = {"Dziś", "Wczoraj", "Ten miesiąc", "Poprzedni miesiąc", "Ten rok", "Wszystko"};
    const char *en[RANGE_COUNT] = {"Today", "Yesterday", "This month", "Previous month", "This year", "Everything"};
    for (int i = 0; i < RANGE_COUNT; i++) {
        ui.qr_btn[i] = button(ui.overlay, IS_EN ? en[i] : pl[i], 20, 70 + i * 62, 250, 52, C_CARD2,
                              export_range_cb, (void *)(intptr_t)i);
    }

    lv_obj_t *frame = box(ui.overlay, 294, 64, QR_PX + 12, QR_PX + 12, 0xFFFFFF, 12);
    (void)frame;
    ui.qr_canvas = lv_canvas_create(ui.overlay);
    lv_obj_set_pos(ui.qr_canvas, 300, 70);
    if (qr_buf) lv_canvas_set_buffer(ui.qr_canvas, qr_buf, QR_PX, QR_PX, LV_COLOR_FORMAT_RGB565);

    ui.qr_url = label(ui.overlay, "", &font_pl_18, C_TEXT, 300, 350);
    ui.qr_hint = label(ui.overlay, "", &font_pl_14, C_DIM, 300, 380);
    lv_label_set_long_mode(ui.qr_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui.qr_hint, 480);

    label(ui.overlay, TR("Arkusze: Podsumowanie · Dni · Kwadranse (do 93 dni) · Informacje",
                         "Sheets: Summary · Days · Quarters (up to 93 days) · Info"),
          &font_pl_14, C_MUTED, 300, 440);

    export_refresh();
}


// ============================================================
// ABOUT / CREDITS
// ============================================================

static void about_open()
{
    overlay_close();
    ov_mode = OV_ABOUT;
    ui.overlay = box(scr, 0, 0, 800, 480, C_BG, 0);
    lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_CLICKABLE);
    label(ui.overlay, ICON_SOLAR, &font_pl_32, C_PV, 24, 16);
    label(ui.overlay, "SigDash " SIGDASH_VERSION " (" SIGDASH_BUILD ")", &font_pl_32, C_TEXT, 72, 14);
    button(ui.overlay, LV_SYMBOL_CLOSE, 724, 10, 60, 44, C_CARD2,
           [](lv_event_t *) { lv_async_call([](void *) { overlay_close(); }, nullptr); }, nullptr);

    char t[160];
    snprintf(t, sizeof(t), TR("Autor: %s", "Author: %s"), SIGDASH_AUTHOR);
    label(ui.overlay, t, &font_pl_18, C_TEXT, 26, 64);

    lv_obj_t *d = label(ui.overlay,
        TR("Nieoficjalny panel dla instalacji Sigenergy. Projekt niezwiązany z Sigenergy Technology "
           "ani przez nią niewspierany. Sigenergy, mySigen i SigenStor są znakami towarowymi ich właścicieli. "
           "SigDash tylko odczytuje dane (Modbus TCP, funkcja 0x04) – niczego nie zmienia w falowniku. "
           "Wyliczenia zarobku są szacunkowe; wiążąca jest faktura sprzedawcy energii.",
           "Unofficial dashboard for Sigenergy systems. Not affiliated with or endorsed by Sigenergy Technology. "
           "Sigenergy, mySigen and SigenStor are trademarks of their owners. "
           "SigDash only reads data (Modbus TCP, function 0x04) – it never changes the inverter. "
           "Earnings are estimates; your energy supplier's bill is binding."),
        &font_pl_14, C_DIM, 26, 96);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d, 748);

    lv_obj_t *c = card(24, 196, 752, 268);
    label(c, TR("Wykorzystane oprogramowanie i dane", "Third-party software and data"), &font_pl_18, C_TEXT, 16, 12);
    static const char *lines[] = {
        "LVGL – MIT License  ·  lvgl.io",
        "ESP32_Display_Panel, ESP32_IO_Expander, esp-lib-utils – Apache 2.0  ·  Espressif",
        "Arduino core for ESP32 / ESP-IDF – LGPL 2.1 / Apache 2.0  ·  Espressif",
        "QR Code generator – MIT License  ·  Project Nayuki",
        "Montserrat – SIL Open Font License 1.1  ·  Font Awesome Free – OFL 1.1 / CC BY 4.0",
        "Ceny RCE / RCE prices – PSE S.A. (api.raporty.pse.pl)",
        "Energy-Charts – CC BY 4.0 (Fraunhofer ISE; ENTSO-E, Bundesnetzagentur | SMARD.de)",
        "Sigenergy Modbus Protocol – public documentation, Sigenergy Technology",
    };
    for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
        lv_obj_t *l = label(c, lines[i], &font_pl_14, C_DIM, 16, 48 + i * 26);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, 720);
    }
}


// ============================================================
// BILLING
// ============================================================

// ---------- zone picker (full screen list) ----------
static void zone_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= SIG_ZONE_COUNT) return;
    E.zone = (uint8_t)i;
    sig_req_update_prefs(E);
    E_dirty = 0;
    lv_async_call([](void *) { overlay_close(); go_page(page); }, nullptr);
}

static void zone_picker_open()
{
    overlay_close();
    ov_mode = OV_ZONE;
    ui.overlay = box(scr, 0, 0, 800, 480, C_BG, 0);
    lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_CLICKABLE);
    label(ui.overlay, TR("Kraj / strefa cenowa (Energy-Charts)", "Country / bidding zone (Energy-Charts)"),
          &font_pl_24, C_TEXT, 20, 16);
    button(ui.overlay, LV_SYMBOL_CLOSE, 724, 10, 60, 44, C_CARD2,
           [](lv_event_t *) { lv_async_call([](void *) { overlay_close(); }, nullptr); }, nullptr);

    lv_obj_t *list = lv_obj_create(ui.overlay);
    lv_obj_set_pos(list, 10, 64);
    lv_obj_set_size(list, 780, 408);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_style_pad_column(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);

    for (int i = 0; i < SIG_ZONE_COUNT; i++) {
        bool sel = i == E.zone;
        lv_obj_t *b = lv_button_create(list);
        lv_obj_set_size(b, 180, 56);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(sel ? C_ACCENT : C_CARD2), 0);
        lv_obj_t *c = lv_label_create(b);
        lv_label_set_text(c, SIG_ZONES[i].code);
        lv_obj_set_style_text_font(c, &font_pl_18, 0);
        lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, -4);
        lv_obj_t *n = lv_label_create(b);
        lv_label_set_text(n, IS_EN ? SIG_ZONES[i].en : SIG_ZONES[i].pl);
        lv_obj_set_style_text_font(n, &font_pl_14, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(sel ? C_TEXT : C_DIM), 0);
        lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 0, 4);
        lv_obj_add_event_cb(b, zone_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void zone_button(lv_obj_t *parent, int y)
{
    char t[48];
    const PriceZone &z = SIG_ZONES[E.zone < SIG_ZONE_COUNT ? E.zone : 0];
    snprintf(t, sizeof(t), "%s  " LV_SYMBOL_RIGHT, z.code);
    lv_obj_t *b = button(parent, t, 196, y, 172, 38, C_CARD2, [](lv_event_t *) { zone_picker_open(); }, nullptr);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &font_pl_14, 0);
    label(parent, IS_EN ? z.en : z.pl, &font_pl_14, C_MUTED, 14, y + 22);
}

static void build_billing()
{
    sig_get_settings(E);
    E_dirty = 0;
    const uint8_t src = E.price_src;

    // ---------- mode + price source ----------
    lv_obj_t *a = card(12, 52, 382, 420);
    card_title(a, ICON_COINS, C_GOLD, TR("Tryb i źródło ceny", "Mode & price source"));

    label(a, TR("Karta zarobku na ekranie głównym", "Earnings card on the main screen"), &font_pl_14, C_DIM, 14, 42);
    static const char *emode_pl[2] = {"Zarobek łączny", "Konto prosumenta"};
    static const char *emode_en[2] = {"Total earnings", "Prosumer account"};
    make_seg(a, 14, 62, 354, 40, IS_EN ? emode_en : emode_pl, 2, &E.earn_mode);

    label(a, TR("Źródło ceny sprzedaży", "Export price source"), &font_pl_14, C_DIM, 14, 114);
    static const char *src_pl[3] = {"RCE (PSE)", "Energy-Charts", "Stała"};
    static const char *src_en[3] = {"RCE (PSE)", "Energy-Charts", "Fixed"};
    make_seg(a, 14, 134, 354, 40, IS_EN ? src_en : src_pl, 3, &E.price_src, true);

    int y = 188;
    if (src == SRC_FIXED) {
        label(a, TR("Stała cena sprzedaży", "Fixed export price"), &font_pl_14, C_DIM, 14, y);
        make_stepper(a, y + 20, &E.fixed_export_price, 0.01f, -1.0f, 5.0f, 2, "/kWh");
        y += 74;
    } else {
        label(a, TR("Rozdzielczość ceny", "Price resolution"), &font_pl_14, C_DIM, 14, y + 8);
        static const char *res[2] = {"1 h", "15 min"};
        make_seg(a, 196, y, 172, 36, res, 2, &E.price_res);
        y += 48;
    }

    // country / zone
    if (src == SRC_RCE) {
        label(a, TR("Kraj", "Country"), &font_pl_14, C_DIM, 14, y + 6);
        label_r(a, TR("Polska (PSE)", "Poland (PSE)"), &font_pl_14, C_TEXT, -14, y + 6);
        y += 40;
    } else {
        label(a, src == SRC_EC ? TR("Strefa cenowa", "Bidding zone") : TR("Kraj (strefa czasowa)", "Country (time zone)"),
              &font_pl_14, C_DIM, 14, y);
        zone_button(a, y - 4);
        y += 50;
    }

    if (src == SRC_RCE) {
        label(a, TR("RCE × 1,23 (net-billing)", "RCE × 1.23 (PL net-billing)"), &font_pl_14, C_DIM, 14, y + 6);
        make_switch(a, 312, y, E.export_coef > 1.01f, [](lv_event_t *e) {
            bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
            E.export_coef = on ? 1.23f : 1.0f;
            E_dirty = 1;
        });
        y += 40;
    }

    label(a, TR("Ujemna cena liczona jako 0", "Negative price counts as 0"), &font_pl_14, C_DIM, 14, y + 6);
    make_switch(a, 312, y, E.neg_as_zero, [](lv_event_t *e) {
        E.neg_as_zero = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
        E_dirty = 1;
    });
    y += 44;

    const char *nt =
        src == SRC_RCE ? TR("Rynkowa cena energii z PSE – rozliczenie prosumentów w Polsce.",
                            "Market price from PSE – Polish prosumer settlement.")
      : src == SRC_EC  ? TR("Ceny day-ahead z energy-charts.info (EUR/MWh, przeliczane kursem). Dane: ENTSO-E, Bundesnetzagentur | SMARD.de.",
                            "Day-ahead prices from energy-charts.info (EUR/MWh, converted with the rate). Data: ENTSO-E, Bundesnetzagentur | SMARD.de.")
                       : TR("Stała cena, bez pobierania cen z internetu.", "Fixed price, nothing is downloaded.");
    note(a, nt, y);

    // ---------- prices + deposit ----------
    lv_obj_t *b = card(406, 52, 382, 420);
    card_title(b, ICON_WALLET, C_GOLD, TR("Ceny i depozyt", "Prices & deposit"));

    label(b, TR("Cena zakupu (z dystrybucją) – oszczędność", "Import price (incl. grid fees) – savings"),
          &font_pl_14, C_DIM, 14, 40);
    make_stepper(b, 60, &E.import_price, 0.01f, 0.0f, 5.0f, 2, "/kWh");

    label(b, TR("Cena samej energii – pobór z depozytu", "Energy-only price – paid from deposit"),
          &font_pl_14, C_DIM, 14, 108);
    make_stepper(b, 128, &E.energy_price, 0.01f, 0.0f, 5.0f, 2, "/kWh");

    label(b, TR("Stan konta prosumenta (depozytu)", "Prosumer account (deposit) balance"),
          &font_pl_14, C_DIM, 14, 176);
    ui.dep_val = label(b, "", &font_pl_24, C_GOLD, 14, 198);
    button(b, TR("Ustaw", "Set"), 252, 192, 116, 40, C_CARD2, [](lv_event_t *) {
        char init[24];
        snprintf(init, sizeof(init), "%.2f", M.deposit);
        overlay_open(OV_DEPOSIT, TR("Stan depozytu z faktury", "Deposit balance from your bill"), init);
    }, nullptr);

    label(b, TR("Waluta", "Currency"), &font_pl_14, C_DIM, 14, 246);
    static const char *cur[4] = {"zł", "€", "£", "$"};
    make_seg(b, 14, 266, 354, 36, cur, 4, &E.currency, true);

    if (src == SRC_EC && E.currency != CUR_EUR) {
        char t[48];
        snprintf(t, sizeof(t), TR("Kurs EUR/%s (ceny Energy-Charts)", "Rate EUR/%s (Energy-Charts prices)"),
                 sig_currency(E.currency));
        label(b, t, &font_pl_14, C_DIM, 14, 314);
        make_stepper(b, 334, &E.fx_rate, 0.01f, 0.01f, 50.0f, 2, "");
        // the stepper shows "4,25 zł" = value of 1 EUR
    } else {
        note(b, TR("Depozyt: + oddana energia × cena sprzedaży, − pobrana energia × cena energii. Przepisz stan z faktury.",
                   "Deposit: + exported energy × export price, − imported energy × energy price. Copy the balance from your bill."),
             314);
    }

    lv_obj_t *rs = label(b, TR("Przytrzymaj, aby wyzerować liczniki", "Hold to reset all counters"),
                         &font_pl_14, C_IMPORT, 14, 394);
    lv_obj_add_flag(rs, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rs, [](lv_event_t *e) {
        sig_req_reset_money();
        lv_label_set_text((lv_obj_t *)lv_event_get_target(e), TR("Liczniki wyzerowane", "Counters reset"));
    }, LV_EVENT_LONG_PRESSED, nullptr);
}

static void update_billing()
{
    if (!ui.dep_val) return;
    set_text(ui.dep_val, "%s %s", fmoney(M.deposit), sig_currency(E.currency));
    // currency change must also update the stepper units
    static uint8_t shown_cur = 255;
    if (shown_cur != E.currency) {
        shown_cur = E.currency;
        S.currency = E.currency;
        for (int i = 0; i < stepper_count; i++) stepper_show(steppers[i]);
    }
    flush_edits(false);
}


// ============================================================
// SETTINGS
// ============================================================

static void wifi_item_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= N.scan_count) return;
    strlcpy(ov_ssid, N.scan_ssid[i], sizeof(ov_ssid));
    char t[64];
    snprintf(t, sizeof(t), TR("Hasło do sieci „%s”", "Password for “%s”"), ov_ssid);
    overlay_open(OV_WIFI_PASS, t, "");
}

static void rebuild_wifi_list()
{
    if (!ui.wifi_list) return;
    lv_obj_clean(ui.wifi_list);
    for (int i = 0; i < N.scan_count; i++) {
        lv_obj_t *b = lv_button_create(ui.wifi_list);
        lv_obj_set_size(b, lv_pct(100), 44);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD2), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, N.scan_ssid[i]);
        lv_obj_set_style_text_font(l, &font_pl_18, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_t *r = lv_label_create(b);
        char rs[16];
        snprintf(rs, sizeof(rs), "%d dBm", N.scan_rssi[i]);
        lv_label_set_text(r, rs);
        lv_obj_set_style_text_font(r, &font_pl_14, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(C_DIM), 0);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_event_cb(b, wifi_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    ui.scan_seq_shown = N.scan_seq;
}

static void inv_step_cb(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int v = E.inv_id + step;
    E.inv_id = constrain(v, 1, 246);
    E_dirty = millis();
    set_text(ui.inv_id, "%d", E.inv_id);
}

static void build_settings()
{
    sig_get_settings(E);
    E_dirty = 0;

    // ---------- WiFi ----------
    lv_obj_t *w = card(12, 52, 382, 420);
    card_title(w, ICON_WIFI, C_ACCENT, "Wi-Fi");
    ui.wifi_status = label(w, "", &font_pl_14, C_DIM, 14, 40);
    lv_label_set_long_mode(ui.wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui.wifi_status, 354);

    ui.wifi_scan_btn = button(w, "", 14, 96, 354, 44, C_ACCENT,
                              [](lv_event_t *) { sig_req_wifi_scan(); }, nullptr);

    ui.wifi_list = lv_obj_create(w);
    lv_obj_set_pos(ui.wifi_list, 8, 150);
    lv_obj_set_size(ui.wifi_list, 366, 262);
    lv_obj_set_style_bg_opa(ui.wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.wifi_list, 0, 0);
    lv_obj_set_style_pad_all(ui.wifi_list, 6, 0);
    lv_obj_set_style_pad_row(ui.wifi_list, 6, 0);
    lv_obj_set_flex_flow(ui.wifi_list, LV_FLEX_FLOW_COLUMN);
    rebuild_wifi_list();

    // ---------- Sigenergy ----------
    lv_obj_t *g = card(406, 52, 382, 200);
    card_title(g, ICON_SOLAR, C_PV, TR("Sigenergy (Modbus TCP, odczyt)", "Sigenergy (Modbus TCP, read-only)"));
    ui.sig_ip = label(g, "", &font_pl_24, C_TEXT, 14, 38);
    ui.sig_status = label_dot(g, &font_pl_14, C_DIM, 14, 70, 354);

    char sbt[48];
    snprintf(sbt, sizeof(sbt), "%s  %s", ICON_SEARCH, TR("Szukaj w sieci", "Search network"));
    lv_obj_t *sb = button(g, sbt, 14, 96, 198, 44, C_CARD2, [](lv_event_t *) { sig_req_discover(); }, nullptr);
    lv_obj_set_style_text_font(lv_obj_get_child(sb, 0), &font_pl_14, 0);
    lv_obj_t *ib = button(g, TR("Wpisz IP", "Enter IP"), 222, 96, 146, 44, C_CARD2, [](lv_event_t *) {
        overlay_open(OV_IP, TR("Adres IP Sigenergy", "Sigenergy IP address"), N.sig_ip);
    }, nullptr);
    lv_obj_set_style_text_font(lv_obj_get_child(ib, 0), &font_pl_14, 0);

    label(g, TR("ID falownika (Modbus)", "Inverter ID (Modbus)"), &font_pl_14, C_DIM, 14, 160);
    button(g, "−", 222, 150, 44, 40, C_CARD2, inv_step_cb, (void *)(intptr_t)-1);
    ui.inv_id = label(g, "", &font_pl_18, C_TEXT, 280, 160);
    set_text(ui.inv_id, "%d", E.inv_id);
    button(g, "+", 324, 150, 44, 40, C_CARD2, inv_step_cb, (void *)(intptr_t)1);

    // ---------- language & data ----------
    lv_obj_t *l = card(406, 262, 382, 210);
    card_title(l, ICON_LIST, C_ACCENT, TR("Język i dane", "Language & data"));
    static const char *langs[2] = {"PL", "EN"};
    make_seg(l, 14, 40, 108, 34, langs, 2, &E.lang, true);
    // display pixel clock (live): lower = more margin when WiFi / flash load the memory bus
    static const char *clks[3] = {"LCD 16", "15", "14 MHz"};
    make_seg(l, 132, 40, 236, 34, clks, 3, &E.lcd_clk, false, []() {
        // applied live, saved only after "Keep" - otherwise reverted in 15 s
        E_dirty = 0;
        uint8_t old = S.lcd_clk;
        lcd_set_clock(E.lcd_clk);
        trial_start(TRIAL_CLK, old);
    });
    // bounce buffer (needs restart)
    static const char *bb_pl[3] = {"Bufor mały", "średni", "bez bufora"};
    static const char *bb_en[3] = {"Buffer small", "medium", "no buffer"};
    make_seg(l, 14, 80, 354, 34, IS_EN ? bb_en : bb_pl, 3, &E.lcd_bb, false, []() {
        // saved directly (the board restarts right after); after the restart the user has
        // 15 s to confirm, otherwise the old mode comes back (also after a power cut)
        Preferences p;
        p.begin("sigdash", false);
        p.putUChar("lcdbb_old", S.lcd_bb);
        p.putUChar("lcdbb", E.lcd_bb);
        p.putUChar("lcdbb_try", 1);
        p.end();
        sig_req_update_prefs(E);
        E_dirty = 0;
        g_restarting = true;
        set_text(ui.store_lbl, "%s", TR("Restart płytki…", "Restarting…"));
        lv_timer_create([](lv_timer_t *) { esp_restart(); }, 1500, nullptr);
    });
    ui.store_lbl = label(l, "", &font_pl_14, C_DIM, 14, 120);
    lv_label_set_long_mode(ui.store_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui.store_lbl, 354);
    char eb[64];
    snprintf(eb, sizeof(eb), "%s  %s", ICON_CHART, TR("Eksport (QR)", "Export (QR)"));
    lv_obj_t *xb = button(l, eb, 14, 160, 170, 38, C_ACCENT, [](lv_event_t *) { export_open(); }, nullptr);
    lv_obj_set_style_text_font(lv_obj_get_child(xb, 0), &font_pl_14, 0);
    char ab[64];
    snprintf(ab, sizeof(ab), "v" SIGDASH_VERSION "  ·  %s  " LV_SYMBOL_RIGHT, TR("O programie", "About"));
    lv_obj_t *al = label(l, ab, &font_pl_14, C_ACCENT, 198, 170);
    lv_obj_add_flag(al, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(al, 12);
    lv_obj_add_event_cb(al, [](lv_event_t *) { about_open(); }, LV_EVENT_CLICKED, nullptr);
}

static void update_settings()
{
    if (!ui.wifi_status) return;

    if (N.wifi_ok)
        set_text(ui.wifi_status, TR("Połączono z „%s”\nIP %s  ·  sygnał %d dBm", "Connected to “%s”\nIP %s  ·  signal %d dBm"),
                 N.ssid, N.ip, N.rssi);
    else if (N.wifi_connecting)
        set_text(ui.wifi_status, "%s", TR("Łączenie…", "Connecting…"));
    else if (S.ssid[0])
        set_text(ui.wifi_status, TR("Brak połączenia z „%s”", "Not connected to “%s”"), S.ssid);
    else
        set_text(ui.wifi_status, "%s", TR("Nie skonfigurowano. Wybierz sieć:", "Not configured. Choose a network:"));

    if (N.scanning) set_text(lv_obj_get_child(ui.wifi_scan_btn, 0), "%s", TR("Skanowanie…", "Scanning…"));
    else set_text(lv_obj_get_child(ui.wifi_scan_btn, 0), "%s  %s", ICON_SEARCH, TR("Skanuj sieci", "Scan networks"));
    if (N.scan_seq != ui.scan_seq_shown && !ui.overlay) rebuild_wifi_list();

    set_text(ui.sig_ip, "%s", N.sig_ip[0] ? N.sig_ip : TR("nie znaleziono", "not found"));
    switch (N.sig_state) {
    case SIG_OK:        set_text(ui.sig_status, TR("Połączono · %s", "Connected · %s"), sig_run_state_text(D.run_state, S.lang)); break;
    case SIG_SEARCHING: set_text(ui.sig_status, TR("Szukam… %d/254", "Searching… %d/254"), N.discover_progress); break;
    case SIG_NO_WIFI:   set_text(ui.sig_status, "%s", TR("Czekam na Wi-Fi", "Waiting for Wi-Fi")); break;
    default:            set_text(ui.sig_status, "%s", N.msg); break;
    }

    // storage
    StoreInfo si;
    store_info(si);
    if (g_restarting) {
        // keep the "restarting" text
    } else if (!si.ok) {
        set_text(ui.store_lbl, "%s", TR("Historia wyłączona: brak karty SD i partycji danych (schemat 16M Flash 3MB APP/9.9MB FATFS).",
                                        "History off: no SD card and no data partition (scheme 16M Flash 3MB APP/9.9MB FATFS)."));
        set_color(ui.store_lbl, C_IMPORT);
    } else {
        char since[16] = "–";
        if (si.first_date) snprintf(since, sizeof(since), "%04u-%02u-%02u", (unsigned)(si.first_date / 10000),
                                    (unsigned)(si.first_date / 100 % 100), (unsigned)(si.first_date % 100));
        auto sz = [](uint64_t v, char *out, size_t n) {
            if (v >= 1073741824ULL) snprintf(out, n, "%s GB", fnum(v / 1073741824.0, 1));
            else snprintf(out, n, "%s MB", fnum(v / 1048576.0, 1));
        };
        char used[24], total[24];
        sz(si.used_bytes, used, sizeof(used));
        sz(si.total_bytes, total, sizeof(total));
        set_text(ui.store_lbl, TR("%s  ·  %u dni od %s\nzajęte %s z %s", "%s  ·  %u days since %s\nused %s of %s"),
                 strcmp(si.medium, "SD") == 0 ? TR("Karta SD", "SD card") : TR("Pamięć flash", "Flash memory"),
                 (unsigned)si.days, since, used, total);
        set_color(ui.store_lbl, C_DIM);
    }

    flush_edits(false);
}


// ============================================================
// PAGE SWITCH + PERIODIC UPDATE
// ============================================================

// Display pixel clock from the settings (applied by the driver in the next VSYNC)
static esp_lcd_panel_handle_t g_panel = nullptr;
static uint8_t lcd_clk_applied = 255;
static void lcd_set_clock(uint8_t idx)
{
    if (!g_panel) return;
    uint32_t hz = sig_lcd_clk_hz(idx);
    esp_err_t r = esp_lcd_rgb_panel_set_pclk(g_panel, hz);
    lcd_clk_applied = idx;
    Serial.printf("[LCD] pixel clock %u MHz (%s)\n", (unsigned)(hz / 1000000), r == ESP_OK ? "ok" : esp_err_to_name(r));
}

// ---------- "keep this display setting?" with automatic revert ----------
static int      trial_kind = TRIAL_NONE;
static uint8_t  trial_old = 0;
static int      trial_left = 0;
static lv_obj_t *trial_bar = nullptr, *trial_lbl = nullptr;
static lv_timer_t *trial_tmr = nullptr;

static void trial_end(bool keep)
{
    int kind = trial_kind;
    trial_kind = TRIAL_NONE;
    if (trial_tmr) { lv_timer_delete(trial_tmr); trial_tmr = nullptr; }
    if (trial_bar) { lv_obj_delete(trial_bar); trial_bar = nullptr; trial_lbl = nullptr; }

    if (kind == TRIAL_CLK) {
        if (!keep) {
            E.lcd_clk = trial_old;
            lcd_set_clock(trial_old);
        }
        sig_req_update_prefs(E);
        E_dirty = 0;
        if (page == PAGE_SETTINGS) go_page(page);          // restyle the buttons
    } else if (kind == TRIAL_BB) {
        Preferences p;
        p.begin("sigdash", false);
        if (keep) {
            p.putUChar("lcdbb_try", 0);
        } else {
            uint8_t old = p.getUChar("lcdbb_old", 1);
            p.putUChar("lcdbb", old);
            p.putUChar("lcdbb_try", 0);
            E.lcd_bb = old;
            sig_req_update_prefs(E);
            E_dirty = 0;
        }
        p.end();
        Serial.printf("[LCD] buffer mode %s\n", keep ? "kept" : "reverted - restarting");
        if (!keep) {
            g_restarting = true;
            lv_timer_create([](lv_timer_t *) { esp_restart(); }, 1200, nullptr);
        }
    }
}

static void trial_show()
{
    set_text(trial_lbl, TR("Czy obraz jest poprawny? Powrót za %d s", "Is the picture OK? Reverting in %d s"), trial_left);
}

static void trial_start(int kind, uint8_t old)
{
    if (trial_kind != TRIAL_NONE) {
        // a second change during a trial: keep the first "old" value, just restart the countdown
        // (no page rebuild here - we are inside the button's event)
        if (trial_kind == kind) old = trial_old;
        if (trial_tmr) { lv_timer_delete(trial_tmr); trial_tmr = nullptr; }
        if (trial_bar) { lv_obj_delete(trial_bar); trial_bar = nullptr; trial_lbl = nullptr; }
    }
    trial_kind = kind;
    trial_old = old;
    trial_left = 15;
    trial_bar = box(lv_layer_top(), 120, 396, 560, 70, C_CARD2, 14);
    lv_obj_add_flag(trial_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(trial_bar, 2, 0);
    lv_obj_set_style_border_color(trial_bar, lv_color_hex(C_ACCENT), 0);
    trial_lbl = label(trial_bar, "", &font_pl_18, C_TEXT, 18, 22);
    button(trial_bar, TR("Zostaw", "Keep"), 420, 12, 124, 46, C_ACCENT,
           [](lv_event_t *) { lv_async_call([](void *) { trial_end(true); }, nullptr); }, nullptr);
    trial_show();
    trial_tmr = lv_timer_create([](lv_timer_t *) {
        if (--trial_left <= 0) trial_end(false);
        else trial_show();
    }, 1000, nullptr);
}

static void apply_lcd_clock()
{
    if (trial_kind == TRIAL_CLK) return;                 // live trial in progress
    if (!g_panel || S.lcd_clk == lcd_clk_applied) return;
    lcd_set_clock(S.lcd_clk);
}

static void update_all()
{
    sig_get_data(D);
    sig_get_prices(P);
    sig_get_money(M);
    sig_get_status(N);
    sig_get_settings(S);
    apply_lcd_clock();

    update_topbar();
    switch (page) {
    case PAGE_OVERVIEW:
        update_flow();
        update_earn_card();
        update_price_card();
        update_tiles();
        break;
    case PAGE_DETAILS:  update_details();  break;
    case PAGE_BILLING:  update_billing();  break;
    case PAGE_SETTINGS: update_settings(); break;
    default: break;
    }
}

static void show_page(Page p)
{
    flush_edits(true);                         // pending edits are applied before leaving
    sig_get_settings(S);                       // backend applies edits immediately

    clear_page();
    page = p;
    build_topbar();
    switch (p) {
    case PAGE_OVERVIEW: build_overview(); break;
    case PAGE_DETAILS:  build_details();  break;
    case PAGE_BILLING:  build_billing();  break;
    case PAGE_SETTINGS: build_settings(); break;
    default: break;
    }
    update_all();
}

static void go_async_cb(void *p)
{
    show_page((Page)(intptr_t)p);
}

static void go_page(Page p)
{
    lv_async_call(go_async_cb, (void *)(intptr_t)p);
}

static void update_timer_cb(lv_timer_t *)
{
    update_all();
    static uint32_t last_qr = 0;
    if (ov_mode == OV_EXPORT && millis() - last_qr > 5000) {   // WiFi / IP may change
        last_qr = millis();
        export_refresh();
    }
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("SigDash v" SIGDASH_VERSION " starting...");

    // --------------------------------------------------------
    // BOARD
    // --------------------------------------------------------

    Board *board = new Board();
    if ((board == nullptr) || !board->init()) {
        Serial.println("Board init failed");
        while (true) delay(1000);
    }

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode =
        ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count =
        esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    LCD *lcd = board->getLCD();
    if (lcd == nullptr) {
        Serial.println("LCD not available");
        while (true) delay(1000);
    }

    auto *lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        lcd->configFrameBufferNumber(frame_buffer_count);
        // Bounce buffer preset (Settings). Small buffers keep the copy interrupt short, so the
        // driver's per-frame DMA restart (VSYNC) is not delayed -> no permanent shift.
        Preferences p;
        p.begin("sigdash", false);
        uint8_t bb = p.getUChar("lcdbb", 1);
        uint8_t clk = sig_lcd_clk_idx(p.getUChar("lcdmhz", 16));
        uint8_t tr = p.getUChar("lcdbb_try", 0);
        if (tr >= 2) {
            // the previous start with the new mode was never confirmed (bad picture, power cut) -> back
            bb = p.getUChar("lcdbb_old", 1);
            p.putUChar("lcdbb", bb);
            p.putUChar("lcdbb_try", 0);
            Serial.println("[LCD] new buffer mode not confirmed - reverted");
        } else if (tr == 1) {
            p.putUChar("lcdbb_try", 2);
            g_bb_trial = true;
        }
        p.end();
        if (bb > 2) bb = 1;
        auto *rgb = static_cast<BusRGB *>(lcd_bus);
        if (bb == 2) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            const_cast<esp_lcd_rgb_panel_config_t *>(rgb->getRgbConfig())->bounce_buffer_size_px = 0;
#pragma GCC diagnostic pop
        } else {
            rgb->configRGB_BounceBufferSize(lcd->getFrameWidth() * (bb == 1 ? 10 : 4));
        }
        rgb->configRGB_FreqHz(sig_lcd_clk_hz(clk));
        lcd_clk_applied = clk;
    }

    assert(board->begin());
    g_panel = lcd->getRefreshPanelHandle();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        const esp_lcd_rgb_panel_config_t *rc = static_cast<BusRGB *>(lcd_bus)->getRgbConfig();
#pragma GCC diagnostic pop
        Serial.printf("[LCD] %u MHz, bounce buffer %u px (%u lines), frame buffers %u\n",
                      (unsigned)(rc->timings.pclk_hz / 1000000), (unsigned)rc->bounce_buffer_size_px,
                      (unsigned)(rc->bounce_buffer_size_px / lcd->getFrameWidth()), (unsigned)rc->num_fbs);
    }

    // --------------------------------------------------------
    // STORAGE: SD card (CS = CH422G EXIO4) or internal flash
    // --------------------------------------------------------
    bool sd_cs = false;
    if (board->getIO_Expander() != nullptr) {
        board->getIO_Expander()->getBase()->digitalWrite(4, 0);   // EXIO4 = SD_CS, active low
        sd_cs = true;
    }
    store_begin(sd_cs);

    // backend (WiFi, Modbus, prices) + web server run on core 0
    sig_backend_begin();
    sig_web_begin();


    // --------------------------------------------------------
    // LVGL
    // --------------------------------------------------------

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 16 * 1024;     // 16 KB is plenty for LVGL; saves internal RAM
    adapter_config.task_priority = 2;
    adapter_config.task_core_id = ARDUINO_RUNNING_CORE;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config =
        ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
            lcd, lcd->getFrameWidth(), lcd->getFrameHeight(), rotation);
    disp_config.profile.use_psram = true;
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != nullptr);

    if (board->getTouch() != nullptr) {
        esp_lv_adapter_touch_config_t touch_config =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, board->getTouch());
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != nullptr);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

    scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(scr, &font_pl_14, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);

    // no WiFi configured yet -> start on the settings page
    sig_get_settings(S);
    show_page(S.ssid[0] ? PAGE_OVERVIEW : PAGE_SETTINGS);
    if (!S.ssid[0]) sig_req_wifi_scan();

    lv_timer_create(update_timer_cb, 1000, nullptr);
    if (g_bb_trial) trial_start(TRIAL_BB, 0);

    esp_lv_adapter_unlock();
    Serial.println("SigDash v" SIGDASH_VERSION " (" SIGDASH_BUILD ") READY");
    sig_mem_report("ui ready");
}

void loop()
{
    delay(1000);
}
