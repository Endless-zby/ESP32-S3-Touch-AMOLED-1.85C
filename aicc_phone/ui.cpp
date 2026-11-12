// Round 360x360 UI. Every widget is placed so that its bounding box stays
// inside the circle (radius 180 around the centre). Dark background hides the
// corners that the glass does not show anyway.
#include "ui.h"
#include "ui_strings.h"
#include "config.h"
#include "face_model.h"
#include "face_render.h"
#include "board_io.h"
#include <esp_heap_caps.h>
#include <time.h>
#include <math.h>

LV_FONT_DECLARE(font_cn_18);
LV_FONT_DECLARE(font_cn_24);

static UiCallbacks s_cb;
static UiScreen s_screen = UI_BOOT;

static const uint32_t COL_BG = 0x000000;
static const uint32_t COL_CARD = 0x161A22;
static const uint32_t COL_KEY = 0x1F2430;
static const uint32_t COL_KEY_PR = 0x3A4256;
static const uint32_t COL_TEXT = 0xF2F4F8;
static const uint32_t COL_DIM = 0x8A93A6;
static const uint32_t COL_ACCENT = 0x3D8BFF;
static const uint32_t COL_GREEN = 0x22C55E;
static const uint32_t COL_RED = 0xEF4444;
static const uint32_t COL_ORANGE = 0xF59E0B;

// ---------------------------------------------------------------------------
// widgets
// ---------------------------------------------------------------------------
static lv_obj_t *scr_boot, *boot_status, *boot_spinner;
static lv_obj_t *scr_setup, *setup_ssid, *setup_pass, *setup_url, *setup_status;
static lv_obj_t *scr_dialer, *dial_status_icon, *dial_status_text, *dial_number, *dial_keypad, *dial_call_btn, *dial_del_btn, *dial_set_btn;
static lv_obj_t *scr_call, *call_status, *call_party, *call_timer, *call_bar_mic, *call_bar_spk, *call_mute_btn, *call_mute_lbl, *call_volm_btn, *call_volp_btn, *call_hang_btn, *call_vol_lbl;
static lv_obj_t *scr_in, *in_caller, *in_answer_btn, *in_reject_btn;
static lv_obj_t *scr_set, *set_lines[4], *set_back_btn, *set_reset_btn, *set_vol_lbl, *set_vol_bar, *set_volm_btn, *set_volp_btn;
static lv_obj_t *toast_lbl = NULL;
static lv_timer_t *toast_timer = NULL;
static lv_timer_t *vol_timer = NULL;

static String s_number;        // digits typed so far
static String s_defaultNumber; // number used when nothing typed
static bool s_muted = false;
static int s_volume = DEF_VOLUME;

// ---- face home screen (idle + in-call) ----
static FaceModel s_face;
static FaceRenderer s_faceR;
static uint16_t *s_faceBuf = NULL;
static lv_image_dsc_t s_faceDsc;
static lv_obj_t *scr_face, *face_canvas, *face_clock, *face_batt, *face_batt_fill, *face_batt_pct, *face_ring, *face_hang;
static lv_timer_t *face_timer = NULL;
static bool s_faceCall = false;
static uint32_t s_battAt = 0;
static int s_battPct = -1;

// touch-follow ("pet the face"): the eyes look where the finger drags (full
// deflection after ~95 px), the gaze holds for 200 ms, and a long press closes
// the eyes - same behaviour as the reference PetEyes/HotelPet sketches.
static bool s_faceTouch = false, s_faceLong = false, s_gazeActive = false;
static bool s_listenRing = false;
static int16_t s_faceStartX = 0, s_faceStartY = 0;
static uint32_t s_gazeAt = 0;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static lv_obj_t *mk_screen() {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s, lv_color_hex(COL_TEXT), 0);
  lv_obj_set_style_text_font(s, &font_cn_18, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color, lv_align_t align, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_align(l, align, x, y);
  return l;
}

static lv_obj_t *mk_round_btn(lv_obj_t *parent, int size, uint32_t color, const char *symbol, const lv_font_t *font, int x, int y, lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(b, lv_color_darken(lv_color_hex(color), 60), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, x, y);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, symbol);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

static lv_obj_t *mk_pill_btn(lv_obj_t *parent, int w, int h, uint32_t color, const char *txt, const lv_font_t *font, int x, int y, lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(b, lv_color_darken(lv_color_hex(color), 60), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, x, y);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

// "hang up" glyph = phone handset rotated by 135 degrees around its centre
static void rotate_icon(lv_obj_t *l) {
  lv_obj_set_style_transform_pivot_x(l, LV_PCT(50), 0);
  lv_obj_set_style_transform_pivot_y(l, LV_PCT(50), 0);
  lv_obj_set_style_transform_rotation(l, 1350, 0);
}

static void load(lv_obj_t *scr, UiScreen id) {
  s_screen = id;
  lv_screen_load(scr);
}

static void key_click() { if (s_cb.onKey) s_cb.onKey(); }

// ---------------------------------------------------------------------------
// boot
// ---------------------------------------------------------------------------
static void build_boot() {
  scr_boot = mk_screen();
  mk_label(scr_boot, STR_APP_NAME, &lv_font_montserrat_28, COL_TEXT, LV_ALIGN_CENTER, 0, -70);
  boot_spinner = lv_spinner_create(scr_boot);
  lv_spinner_set_anim_params(boot_spinner, 1200, 200);
  lv_obj_set_size(boot_spinner, 56, 56);
  lv_obj_set_style_arc_color(boot_spinner, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(boot_spinner, lv_color_hex(COL_KEY), LV_PART_MAIN);
  lv_obj_set_style_arc_width(boot_spinner, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(boot_spinner, 6, LV_PART_MAIN);
  lv_obj_align(boot_spinner, LV_ALIGN_CENTER, 0, 5);
  boot_status = mk_label(scr_boot, STR_BOOTING, &font_cn_18, COL_DIM, LV_ALIGN_CENTER, 0, 70);
}

// ---------------------------------------------------------------------------
// setup / provisioning
// ---------------------------------------------------------------------------
static void build_setup() {
  scr_setup = mk_screen();
  mk_label(scr_setup, LV_SYMBOL_WIFI, &font_cn_24, COL_ACCENT, LV_ALIGN_CENTER, 0, -128);
  mk_label(scr_setup, STR_SETUP_TITLE, &font_cn_24, COL_TEXT, LV_ALIGN_CENTER, 0, -95);
  mk_label(scr_setup, STR_SETUP_STEP1, &font_cn_18, COL_DIM, LV_ALIGN_CENTER, 0, -58);
  setup_ssid = mk_label(scr_setup, "AICC-Phone-XXXX", &lv_font_montserrat_20, COL_TEXT, LV_ALIGN_CENTER, 0, -32);
  setup_pass = mk_label(scr_setup, STR_SETUP_PASS ": 12345678", &font_cn_18, COL_DIM, LV_ALIGN_CENTER, 0, -6);
  mk_label(scr_setup, STR_SETUP_STEP2, &font_cn_18, COL_DIM, LV_ALIGN_CENTER, 0, 28);
  setup_url = mk_label(scr_setup, "192.168.4.1", &lv_font_montserrat_24, COL_ACCENT, LV_ALIGN_CENTER, 0, 56);
  setup_status = mk_label(scr_setup, STR_SETUP_WAITING, &font_cn_18, COL_ORANGE, LV_ALIGN_CENTER, 0, 105);
  lv_obj_set_width(setup_status, 230);
  lv_label_set_long_mode(setup_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(setup_status, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------------------
// dialer
// ---------------------------------------------------------------------------
static void update_number_label() {
  if (s_number.length()) {
    lv_label_set_text(dial_number, s_number.c_str());
    lv_obj_set_style_text_color(dial_number, lv_color_hex(COL_TEXT), 0);
  } else {
    lv_label_set_text(dial_number, s_defaultNumber.length() ? s_defaultNumber.c_str() : "---");
    lv_obj_set_style_text_color(dial_number, lv_color_hex(COL_DIM), 0);
  }
}

static void keypad_event(lv_event_t *e) {
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(obj);
  if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
  const char *txt = lv_buttonmatrix_get_button_text(obj, id);
  if (!txt) return;
  if (s_number.length() < 24) s_number += txt;
  update_number_label();
  key_click();
}

static void del_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_SHORT_CLICKED) {
    if (s_number.length()) s_number.remove(s_number.length() - 1);
  } else if (code == LV_EVENT_LONG_PRESSED) {
    s_number = "";
  } else return;
  update_number_label();
  key_click();
}

static void call_event(lv_event_t *) {
  key_click();
  String n = s_number.length() ? s_number : s_defaultNumber;
  if (n.length() == 0) return;
  if (s_cb.onDial) s_cb.onDial(n.c_str());
}

static void settings_event(lv_event_t *) {
  key_click();
  // the application fills in the details and switches the screen
  if (s_cb.onSettings) s_cb.onSettings();
}

static const char *keymap[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n", "*", "0", "#", ""};

static void build_dialer() {
  scr_dialer = mk_screen();

  // status row (y = 30, max width ~190)
  dial_status_icon = mk_label(scr_dialer, LV_SYMBOL_WIFI, &font_cn_18, COL_DIM, LV_ALIGN_TOP_MID, -46, 20);
  dial_status_text = mk_label(scr_dialer, STR_SIP_DIRECT, &font_cn_18, COL_DIM, LV_ALIGN_TOP_MID, 14, 20);

  // number display
  dial_number = mk_label(scr_dialer, "", &lv_font_montserrat_28, COL_DIM, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_set_width(dial_number, 270);
  lv_obj_set_style_text_align(dial_number, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(dial_number, LV_LABEL_LONG_CLIP);

  // keypad 222x190, y 92..282
  dial_keypad = lv_buttonmatrix_create(scr_dialer);
  lv_buttonmatrix_set_map(dial_keypad, keymap);
  lv_buttonmatrix_set_button_ctrl_all(dial_keypad, LV_BUTTONMATRIX_CTRL_NO_REPEAT);  // one digit per tap, even when held
  lv_obj_set_size(dial_keypad, 222, 190);
  lv_obj_align(dial_keypad, LV_ALIGN_TOP_MID, 0, 92);
  lv_obj_set_style_bg_opa(dial_keypad, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(dial_keypad, 0, 0);
  lv_obj_set_style_pad_all(dial_keypad, 0, 0);
  lv_obj_set_style_pad_row(dial_keypad, 6, 0);
  lv_obj_set_style_pad_column(dial_keypad, 8, 0);
  lv_obj_set_style_bg_color(dial_keypad, lv_color_hex(COL_KEY), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(dial_keypad, lv_color_hex(COL_KEY_PR), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_radius(dial_keypad, 14, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(dial_keypad, 0, LV_PART_ITEMS);
  lv_obj_set_style_text_font(dial_keypad, &lv_font_montserrat_24, LV_PART_ITEMS);
  lv_obj_set_style_text_color(dial_keypad, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
  lv_obj_add_event_cb(dial_keypad, keypad_event, LV_EVENT_VALUE_CHANGED, NULL);

  // bottom row centred on y = 310
  dial_del_btn = mk_pill_btn(scr_dialer, 50, 40, COL_KEY, LV_SYMBOL_BACKSPACE, &font_cn_18, -68, 130, NULL);
  lv_obj_add_event_cb(dial_del_btn, del_event, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(dial_del_btn, del_event, LV_EVENT_LONG_PRESSED, NULL);
  dial_call_btn = mk_pill_btn(scr_dialer, 76, 50, COL_GREEN, LV_SYMBOL_CALL, &font_cn_24, 0, 130, call_event);
  dial_set_btn = mk_pill_btn(scr_dialer, 50, 40, COL_KEY, LV_SYMBOL_SETTINGS, &font_cn_18, 68, 130, settings_event);
  update_number_label();
}

// ---------------------------------------------------------------------------
// in-call
// ---------------------------------------------------------------------------
static void mute_event(lv_event_t *) {
  s_muted = !s_muted;
  ui_set_muted(s_muted);
  key_click();
  if (s_cb.onMute) s_cb.onMute(s_muted);
}

static void vol_hide_cb(lv_timer_t *) {
  lv_obj_add_flag(call_vol_lbl, LV_OBJ_FLAG_HIDDEN);
  lv_timer_delete(vol_timer);
  vol_timer = NULL;
}

static void show_volume_label() {
  lv_label_set_text_fmt(call_vol_lbl, LV_SYMBOL_VOLUME_MAX " " STR_VOLUME " %d%%", s_volume);
  lv_obj_clear_flag(call_vol_lbl, LV_OBJ_FLAG_HIDDEN);
  if (vol_timer) lv_timer_reset(vol_timer);
  else vol_timer = lv_timer_create(vol_hide_cb, 1500, NULL);
}

static void volm_event(lv_event_t *) { key_click(); if (s_cb.onVolume) s_cb.onVolume(-10); }
static void volp_event(lv_event_t *) { key_click(); if (s_cb.onVolume) s_cb.onVolume(+10); }
static void hang_event(lv_event_t *) { key_click(); if (s_cb.onHangup) s_cb.onHangup(); }

static lv_obj_t *mk_bar(lv_obj_t *parent, uint32_t color, int y) {
  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_set_size(b, 150, 6);
  lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
  lv_bar_set_range(b, 0, 100);
  lv_bar_set_value(b, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_KEY), LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
  return b;
}

static void build_call() {
  scr_call = mk_screen();
  call_status = mk_label(scr_call, STR_CALLING, &font_cn_18, COL_DIM, LV_ALIGN_TOP_MID, 0, 42);
  call_party = mk_label(scr_call, "", &lv_font_montserrat_28, COL_TEXT, LV_ALIGN_TOP_MID, 0, 74);
  lv_obj_set_width(call_party, 260);
  lv_obj_set_style_text_align(call_party, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(call_party, LV_LABEL_LONG_DOT);
  call_timer = mk_label(scr_call, "00:00", &lv_font_montserrat_20, COL_DIM, LV_ALIGN_TOP_MID, 0, 118);
  // level meters
  mk_label(scr_call, LV_SYMBOL_AUDIO, &font_cn_18, COL_DIM, LV_ALIGN_CENTER, -95, -22);
  call_bar_spk = mk_bar(scr_call, COL_ACCENT, -22);
  mk_label(scr_call, LV_SYMBOL_UP, &font_cn_18, COL_DIM, LV_ALIGN_CENTER, -95, 0);
  call_bar_mic = mk_bar(scr_call, COL_GREEN, 0);
  call_vol_lbl = mk_label(scr_call, "", &font_cn_18, COL_ORANGE, LV_ALIGN_CENTER, 0, -50);
  lv_obj_add_flag(call_vol_lbl, LV_OBJ_FLAG_HIDDEN);

  // control row centred on y = 225 (offset +45)
  call_mute_btn = mk_round_btn(scr_call, 56, COL_KEY, LV_SYMBOL_MUTE, &font_cn_24, -84, 45, mute_event);
  call_mute_lbl = lv_obj_get_child(call_mute_btn, 0);
  call_volm_btn = mk_round_btn(scr_call, 56, COL_KEY, LV_SYMBOL_VOLUME_MID, &font_cn_24, 0, 45, volm_event);
  call_volp_btn = mk_round_btn(scr_call, 56, COL_KEY, LV_SYMBOL_VOLUME_MAX, &font_cn_24, 84, 45, volp_event);
  // hang-up centred on y = 300 (offset +120)
  call_hang_btn = mk_round_btn(scr_call, 72, COL_RED, LV_SYMBOL_CALL, &font_cn_24, 0, 120, hang_event);
  rotate_icon(lv_obj_get_child(call_hang_btn, 0));
}

// ---------------------------------------------------------------------------
// incoming
// ---------------------------------------------------------------------------
static void answer_event(lv_event_t *) { key_click(); if (s_cb.onAnswer) s_cb.onAnswer(); }
static void reject_event(lv_event_t *) { key_click(); if (s_cb.onReject) s_cb.onReject(); }

static void build_incoming() {
  scr_in = mk_screen();
  mk_label(scr_in, LV_SYMBOL_BELL, &font_cn_24, COL_ORANGE, LV_ALIGN_TOP_MID, 0, 38);
  mk_label(scr_in, STR_INCOMING, &font_cn_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 72);
  in_caller = mk_label(scr_in, "", &font_cn_24, COL_ACCENT, LV_ALIGN_TOP_MID, 0, 120);
  lv_obj_set_width(in_caller, 260);
  lv_obj_set_style_text_align(in_caller, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(in_caller, LV_LABEL_LONG_DOT);
  in_reject_btn = mk_round_btn(scr_in, 72, COL_RED, LV_SYMBOL_CALL, &font_cn_24, -62, 100, reject_event);
  rotate_icon(lv_obj_get_child(in_reject_btn, 0));
  in_answer_btn = mk_round_btn(scr_in, 72, COL_GREEN, LV_SYMBOL_CALL, &font_cn_24, 62, 100, answer_event);
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------
static void back_event(lv_event_t *) { key_click(); ui_show_face(); }
static void reset_event(lv_event_t *) { key_click(); if (s_cb.onResetWifi) s_cb.onResetWifi(); }

static void set_volm_event(lv_event_t *) { key_click(); if (s_cb.onVolume) s_cb.onVolume(-10); }
static void set_volp_event(lv_event_t *) { key_click(); if (s_cb.onVolume) s_cb.onVolume(+10); }

static void update_settings_volume() {
  if (!set_vol_lbl) return;
  lv_label_set_text_fmt(set_vol_lbl, LV_SYMBOL_VOLUME_MAX " " STR_VOLUME " %d%%", s_volume);
  lv_bar_set_value(set_vol_bar, s_volume, LV_ANIM_ON);
}

static void build_settings() {
  scr_set = mk_screen();
  mk_label(scr_set, LV_SYMBOL_SETTINGS " " STR_SETTINGS, &font_cn_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 34);
  for (int i = 0; i < 4; i++) {
    set_lines[i] = mk_label(scr_set, "", &font_cn_18, COL_DIM, LV_ALIGN_TOP_MID, 0, 76 + i * 26);
    lv_obj_set_width(set_lines[i], 250);
    lv_obj_set_style_text_align(set_lines[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(set_lines[i], LV_LABEL_LONG_DOT);
  }
  // volume row (centre y = 180 + 14)
  set_volm_btn = mk_round_btn(scr_set, 44, COL_KEY, LV_SYMBOL_MINUS, &font_cn_18, -95, 14, set_volm_event);
  set_volp_btn = mk_round_btn(scr_set, 44, COL_KEY, LV_SYMBOL_PLUS, &font_cn_18, 95, 14, set_volp_event);
  set_vol_bar = lv_bar_create(scr_set);
  lv_obj_set_size(set_vol_bar, 110, 10);
  lv_obj_align(set_vol_bar, LV_ALIGN_CENTER, 0, 14);
  lv_bar_set_range(set_vol_bar, 0, 100);
  lv_obj_set_style_bg_color(set_vol_bar, lv_color_hex(COL_KEY), LV_PART_MAIN);
  lv_obj_set_style_bg_color(set_vol_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_radius(set_vol_bar, 5, LV_PART_MAIN);
  lv_obj_set_style_radius(set_vol_bar, 5, LV_PART_INDICATOR);
  set_vol_lbl = mk_label(scr_set, "", &font_cn_18, COL_TEXT, LV_ALIGN_CENTER, 0, 44);
  update_settings_volume();
  set_reset_btn = mk_pill_btn(scr_set, 170, 42, COL_ORANGE, LV_SYMBOL_WIFI " " STR_RESET_WIFI, &font_cn_18, 0, 82, reset_event);
  set_back_btn = mk_pill_btn(scr_set, 126, 40, COL_KEY, LV_SYMBOL_LEFT " " STR_BACK, &font_cn_18, 0, 128, back_event);
}


// ---------------------------------------------------------------------------
// face home screen
// ---------------------------------------------------------------------------
static void face_tap_event(lv_event_t *) {
  if (s_faceCall && s_cb.onHangup) { key_click(); s_cb.onHangup(); }
}

static void face_touch_point(int16_t &x, int16_t &y) {
  x = y = 0;
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  x = p.x; y = p.y;
}

static void face_gaze_event(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  int16_t x = 0, y = 0;
  face_touch_point(x, y);
  if (code == LV_EVENT_PRESSED) {
    s_faceTouch = true;
    s_faceLong = false;
    s_faceStartX = x;
    s_faceStartY = y;
  } else if (code == LV_EVENT_PRESSING) {
    if (!s_faceTouch) return;
    const float dx = (float)(x - s_faceStartX), dy = (float)(y - s_faceStartY);
    const float dist = sqrtf(dx * dx + dy * dy);
    if (dist > 6.f) {
      const float k = min(1.f, (dist - 6.f) / 95.f) / dist;
      s_face.setGazeOverride(true, dx * k, dy * k);
      s_gazeAt = millis();
      s_gazeActive = true;
    }
  } else if (code == LV_EVENT_LONG_PRESSED) {
    s_faceLong = true;
    s_face.setForceClosed(true);   // long press = closed eyes (petting)
  } else {   // RELEASED / PRESS_LOST
    s_faceTouch = false;
    if (s_faceLong) { s_faceLong = false; s_face.setForceClosed(false); }
  }
}

static void face_hang_event(lv_event_t *) {
  key_click();
  if (s_cb.onHangup) s_cb.onHangup();
}

static void face_clock_update() {
  if (!face_clock) return;
  time_t t = time(nullptr);
  if (t < 1700000000) { lv_label_set_text(face_clock, "--:--"); return; }
  struct tm ti;
  localtime_r(&t, &ti);
  lv_label_set_text_fmt(face_clock, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

static void face_battery_update(bool force) {
  if (!face_batt_fill) return;
  float v = battery_volts();
  bool present = v > 3.0f;
  int pct = 0;
  if (present) {
    pct = (int)((v - 3.30f) * 100.f / 0.90f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
  }
  if (!force && present && pct == s_battPct) return;
  s_battPct = present ? pct : -1;
  lv_obj_set_width(face_batt_fill, pct > 0 ? (pct * 46 / 100 + 2) : 0);
  lv_obj_set_style_bg_color(face_batt_fill, lv_color_hex(pct <= 20 ? COL_RED : COL_TEXT), 0);
  lv_obj_set_style_bg_opa(face_batt_fill, pct <= 20 ? LV_OPA_80 : LV_OPA_50, 0);
  lv_obj_set_style_border_color(face_batt, lv_color_hex(present ? COL_DIM : COL_RED), 0);
  if (face_batt_pct) {
    if (present) lv_label_set_text_fmt(face_batt_pct, "%d", pct);
    else lv_label_set_text(face_batt_pct, "--");
    lv_obj_set_style_text_color(face_batt_pct, lv_color_hex(pct <= 20 ? COL_RED : COL_TEXT), 0);
  }
}

static void face_timer_cb(lv_timer_t *) {
  if (!s_faceBuf || lv_screen_active() != scr_face) return;
  s_face.update(0.033f);
  IRect rects[4];
  int n = s_faceR.render(s_face.eyes(), s_face.brows(), rects, 4);
  for (int i = 0; i < n; i++) {
    lv_area_t a = { rects[i].x0, rects[i].y0, rects[i].x1 - 1, rects[i].y1 - 1 };
    lv_obj_invalidate_area(face_canvas, &a);
  }
  static uint32_t lastClock = 0;
  const uint32_t now = millis();
  if (s_gazeActive && now - s_gazeAt > 200) {
    s_gazeActive = false;
    s_face.setGazeOverride(false, 0, 0);   // back to the idle gaze wander
  }
  if (s_listenRing && face_ring) {
    const float ph = (float)(now % 1600) * (2.f * (float)M_PI / 1600.f);   // 1.6 s breath
    const float b = 0.5f + 0.5f * sinf(ph);
    lv_obj_set_style_border_opa(face_ring, (lv_opa_t)(40.f + 170.f * b), 0);
    lv_obj_set_style_shadow_opa(face_ring, (lv_opa_t)(20.f + 130.f * b), 0);
  }
  if (now - lastClock >= 1000) { lastClock = now; face_clock_update(); }
  if (now - s_battAt > 5000) { s_battAt = now; face_battery_update(false); }
}

static void build_face() {
  scr_face = mk_screen();
  size_t bytes = (size_t)LCD_W * LCD_H * 2;
  s_faceBuf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!s_faceBuf) {
    Serial.println("[UI] face buffer alloc failed");
    mk_label(scr_face, "face", &font_cn_18, COL_DIM, LV_ALIGN_CENTER, 0, 0);
    return;
  }
  memset(s_faceBuf, 0, bytes);
  if (!s_faceR.begin(s_faceBuf, LCD_W, LCD_H)) Serial.println("[UI] face renderer init failed");
  memset(&s_faceDsc, 0, sizeof(s_faceDsc));
  s_faceDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_faceDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  s_faceDsc.header.w = LCD_W;
  s_faceDsc.header.h = LCD_H;
  s_faceDsc.data = (const uint8_t *)s_faceBuf;
  s_faceDsc.data_size = bytes;
  face_canvas = lv_image_create(scr_face);
  lv_image_set_src(face_canvas, &s_faceDsc);
  lv_obj_set_pos(face_canvas, 0, 0);
  lv_obj_add_flag(face_canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(face_canvas, face_tap_event, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(face_canvas, face_gaze_event, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(face_canvas, face_gaze_event, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(face_canvas, face_gaze_event, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(face_canvas, face_gaze_event, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(face_canvas, face_gaze_event, LV_EVENT_PRESS_LOST, NULL);

  face_clock = mk_label(scr_face, "--:--", &lv_font_montserrat_24, COL_DIM, LV_ALIGN_TOP_MID, 0, 12);
  face_batt = lv_obj_create(scr_face);
  lv_obj_set_size(face_batt, 54, 22);
  lv_obj_align(face_batt, LV_ALIGN_BOTTOM_MID, 0, -28);   // bottom centre, inside the round glass
  lv_obj_set_style_bg_opa(face_batt, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(face_batt, 2, 0);
  lv_obj_set_style_border_color(face_batt, lv_color_hex(COL_DIM), 0);
  lv_obj_set_style_radius(face_batt, 5, 0);
  lv_obj_set_style_pad_all(face_batt, 0, 0);
  lv_obj_clear_flag(face_batt, LV_OBJ_FLAG_SCROLLABLE);
  face_batt_fill = lv_obj_create(face_batt);
  lv_obj_set_size(face_batt_fill, 2, 14);
  lv_obj_align(face_batt_fill, LV_ALIGN_LEFT_MID, 3, 0);
  lv_obj_set_style_bg_color(face_batt_fill, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_style_border_width(face_batt_fill, 0, 0);
  lv_obj_set_style_radius(face_batt_fill, 3, 0);
  face_batt_pct = lv_label_create(face_batt);
  lv_label_set_text(face_batt_pct, "--");
  lv_obj_set_style_text_font(face_batt_pct, &font_cn_18, 0);
  lv_obj_set_style_text_color(face_batt_pct, lv_color_hex(COL_TEXT), 0);
  lv_obj_align(face_batt_pct, LV_ALIGN_CENTER, 0, 0);

  // purple breathing ring around the rim while the wake word waits for a command
  face_ring = lv_obj_create(scr_face);
  lv_obj_set_size(face_ring, LCD_W - 16, LCD_H - 16);
  lv_obj_center(face_ring);
  lv_obj_set_style_bg_opa(face_ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(face_ring, 6, 0);
  lv_obj_set_style_border_color(face_ring, lv_color_hex(0x8B5CF6), 0);
  lv_obj_set_style_border_opa(face_ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_color(face_ring, lv_color_hex(0x8B5CF6), 0);
  lv_obj_set_style_shadow_width(face_ring, 24, 0);
  lv_obj_set_style_shadow_spread(face_ring, 2, 0);
  lv_obj_set_style_shadow_opa(face_ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(face_ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(face_ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(face_ring, LV_OBJ_FLAG_SCROLLABLE);

  face_hang = mk_round_btn(scr_face, 64, COL_RED, LV_SYMBOL_CALL, &font_cn_24, 0, 118, face_hang_event);
  lv_obj_add_flag(face_hang, LV_OBJ_FLAG_HIDDEN);

  s_face.begin();
  s_face.setExpression(EX_LISTENING);
  face_timer = lv_timer_create(face_timer_cb, 33, NULL);
}

// ---------------------------------------------------------------------------
// toast
// ---------------------------------------------------------------------------
static void toast_hide(lv_timer_t *) {
  if (toast_lbl) { lv_obj_delete(toast_lbl); toast_lbl = NULL; }
  lv_timer_delete(toast_timer);
  toast_timer = NULL;
}

void ui_toast(const char *msg, uint32_t ms) {
  if (toast_lbl) { lv_obj_delete(toast_lbl); toast_lbl = NULL; }
  if (toast_timer) { lv_timer_delete(toast_timer); toast_timer = NULL; }
  toast_lbl = lv_label_create(lv_layer_top());
  lv_label_set_text(toast_lbl, msg);
  lv_obj_set_style_text_font(toast_lbl, &font_cn_18, 0);
  lv_obj_set_style_text_color(toast_lbl, lv_color_white(), 0);
  lv_obj_set_style_bg_color(toast_lbl, lv_color_hex(0x2B3140), 0);
  lv_obj_set_style_bg_opa(toast_lbl, LV_OPA_90, 0);
  lv_obj_set_style_pad_hor(toast_lbl, 16, 0);
  lv_obj_set_style_pad_ver(toast_lbl, 8, 0);
  lv_obj_set_style_radius(toast_lbl, 16, 0);
  lv_obj_align(toast_lbl, LV_ALIGN_CENTER, 0, 0);
  toast_timer = lv_timer_create(toast_hide, ms, NULL);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
void ui_init(const UiCallbacks &cb) {
  s_cb = cb;
  build_boot();
  build_setup();
  build_dialer();
  build_call();
  build_incoming();
  build_settings();
  build_face();
  load(scr_boot, UI_BOOT);
}

UiScreen ui_screen() { return s_screen; }

void ui_show_boot(const char *status) { ui_boot_status(status); load(scr_boot, UI_BOOT); }
void ui_boot_status(const char *status) { lv_label_set_text(boot_status, status); }

void ui_show_setup(const char *apSsid, const char *apPass, const char *url) {
  lv_label_set_text(setup_ssid, apSsid);
  lv_label_set_text_fmt(setup_pass, STR_SETUP_PASS ": %s", apPass);
  lv_label_set_text(setup_url, url);
  lv_label_set_text(setup_status, STR_SETUP_WAITING);
  load(scr_setup, UI_SETUP);
}
void ui_setup_status(const char *status) { lv_label_set_text(setup_status, status); }

void ui_show_dialer() {
  s_number = "";
  update_number_label();
  load(scr_dialer, UI_DIALER);
}

void ui_set_default_number(const char *number) {
  s_defaultNumber = number;
  if (dial_number) update_number_label();
}

void ui_set_net_status(bool wifiOk, const char *sipText, uint32_t colorHex) {
  lv_label_set_text(dial_status_icon, wifiOk ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
  lv_obj_set_style_text_color(dial_status_icon, lv_color_hex(wifiOk ? COL_GREEN : COL_RED), 0);
  lv_label_set_text(dial_status_text, sipText);
  lv_obj_set_style_text_color(dial_status_text, lv_color_hex(colorHex), 0);
}

void ui_show_call(const char *party, const char *status) {
  lv_label_set_text(call_party, party);
  lv_label_set_text(call_status, status);
  lv_label_set_text(call_timer, "");
  lv_bar_set_value(call_bar_mic, 0, LV_ANIM_OFF);
  lv_bar_set_value(call_bar_spk, 0, LV_ANIM_OFF);
  s_muted = false;
  ui_set_muted(false);
  load(scr_call, UI_CALL);
}
void ui_call_status(const char *status) { lv_label_set_text(call_status, status); }
void ui_call_timer(uint32_t seconds, bool show) {
  if (!show) { lv_label_set_text(call_timer, ""); return; }
  lv_label_set_text_fmt(call_timer, "%02lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}
void ui_call_levels(uint8_t mic, uint8_t spk) {
  lv_bar_set_value(call_bar_mic, mic, LV_ANIM_OFF);
  lv_bar_set_value(call_bar_spk, spk, LV_ANIM_OFF);
}
void ui_set_muted(bool muted) {
  s_muted = muted;
  lv_obj_set_style_bg_color(call_mute_btn, lv_color_hex(muted ? COL_ORANGE : COL_KEY), 0);
}
void ui_set_volume(int percent) {
  s_volume = percent;
  if (s_screen == UI_CALL) show_volume_label();
  update_settings_volume();
}

void ui_show_incoming(const char *caller) {
  lv_label_set_text(in_caller, caller);
  load(scr_in, UI_INCOMING);
}

void ui_show_settings(const char *ip, const char *server, const char *sipState, const char *apName, const char *version) {
  lv_label_set_text_fmt(set_lines[0], STR_IP_ADDR ": %s", ip);
  lv_label_set_text_fmt(set_lines[1], STR_SERVER ": %s", server);
  lv_label_set_text_fmt(set_lines[2], STR_STATUS ": %s", sipState);
  lv_label_set_text_fmt(set_lines[3], STR_VERSION " %s  " STR_HOTSPOT " %s", version, apName);
  load(scr_set, UI_SETTINGS);
}

void ui_refresh_now() { lv_refr_now(NULL); }

// ---------------------------------------------------------------------------
// face public API
// ---------------------------------------------------------------------------
void ui_show_face() {
  if (!s_faceBuf) return;
  load(scr_face, UI_FACE);
  memset(s_faceBuf, 0, (size_t)LCD_W * LCD_H * 2);
  s_faceR.resetDirty();
  s_face.setGazeOverride(false, 0, 0);
  s_face.setForceClosed(false);
  s_gazeActive = false;
  s_faceLong = false;
  lv_obj_invalidate(face_canvas);
  face_clock_update();
  face_battery_update(true);
}

void ui_face_set_expr(int exprId) { s_face.setExpression(exprId); }

void ui_face_call_active(bool active) {
  s_faceCall = active;
  if (!face_hang) return;
  if (active) lv_obj_clear_flag(face_hang, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(face_hang, LV_OBJ_FLAG_HIDDEN);
}

void ui_face_listening(bool on) {
  s_listenRing = on;
  if (face_ring) {
    lv_obj_set_style_border_opa(face_ring, on ? LV_OPA_60 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(face_ring, on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
  }
}

void ui_face_levels(uint8_t mic, uint8_t spk) {
  if (ui_screen() != UI_FACE) return;
  if (spk > 8) s_face.setExpression(EX_SPEAKING);
  else if (mic > 12) s_face.setExpression(EX_USER_SPEAKING);
  else s_face.setExpression(EX_LISTENING);
}
