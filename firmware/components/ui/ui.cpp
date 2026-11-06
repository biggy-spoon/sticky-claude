#include "ui.h"

#include <cstdio>
#include <functional>
#include <utility>

#include "board_config.h"
#include "epaper_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace ui {
namespace {

constexpr const char* kTag = "ui";
constexpr uint32_t kTickPeriodMs = 5;
constexpr uint32_t kUiTaskStackBytes = 8192;
constexpr lv_coord_t kScreenWidth = BOARD_EPD_WIDTH;
constexpr lv_coord_t kScreenHeight = BOARD_EPD_HEIGHT;
constexpr lv_coord_t kMargin = 48;
constexpr lv_coord_t kContentWidth = kScreenWidth - 2 * kMargin;

constexpr lv_coord_t kViewMargin = 28;
constexpr lv_coord_t kViewWidth = kScreenWidth - 2 * kViewMargin;
constexpr lv_coord_t kViewHeaderHeight = 68;
constexpr lv_coord_t kRowMinHeight = 68;  // comfortably larger than a fingertip
constexpr lv_coord_t kCheckboxSize = 30;
constexpr lv_coord_t kIndicatorWidth = 90;  // room reserved in the header for the dots
constexpr lv_coord_t kPageOverlap = 28;     // detail text kept on screen when paging through it
// A row takes about 800 bytes of LVGL's fixed pool, and running out of it crashes
// the device, so a very long list stops here rather than drawing every row.
constexpr size_t kMinLvglFreeForRow = 3000;

struct RowBand {
    lv_coord_t y1;
    lv_coord_t y2;
};

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
esp_timer_handle_t s_tick_timer = nullptr;

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_lock = nullptr;  // guards everything in this block
std::function<void()> s_pending;
bool s_rendering = false;
bool s_device_view_shown = false;
std::vector<RowBand> s_row_bands;

lv_obj_t* s_screen_idle = nullptr;
lv_obj_t* s_screen_recording = nullptr;
lv_obj_t* s_screen_thinking = nullptr;
lv_obj_t* s_screen_reply = nullptr;
lv_obj_t* s_screen_error = nullptr;
lv_obj_t* s_screen_loading = nullptr;
lv_obj_t* s_screen_device_view = nullptr;
lv_obj_t* s_screen_detail = nullptr;

lv_obj_t* s_idle_status_label = nullptr;
lv_obj_t* s_reply_transcript_label = nullptr;
lv_obj_t* s_reply_body_label = nullptr;
lv_obj_t* s_error_message_label = nullptr;
lv_obj_t* s_device_view_title = nullptr;
lv_obj_t* s_device_view_position = nullptr;
lv_obj_t* s_device_view_list = nullptr;
lv_obj_t* s_detail_area = nullptr;
lv_obj_t* s_detail_title = nullptr;
lv_obj_t* s_detail_body = nullptr;
size_t s_rendered_rows = 0;

bool s_idle_wifi = false;
int s_idle_pages = -1;

// Page position: a window of 3 dots with the current page filled, and an arrow
// on each side that has more pages beyond the window.
constexpr int kPageDotCount = 3;
constexpr lv_coord_t kPageDotSize = 12;

struct PageIndicator {
    lv_obj_t* root = nullptr;
    lv_obj_t* left = nullptr;
    lv_obj_t* dots[kPageDotCount] = {};
    lv_obj_t* right = nullptr;
};

PageIndicator s_idle_indicator;
PageIndicator s_reply_indicator;
PageIndicator s_view_indicator;
PageIndicator s_detail_indicator;

void SetHidden(lv_obj_t* obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void SetPageIndicator(const PageIndicator& indicator, PagePos page) {
    SetHidden(indicator.root, page.total <= 1);
    if (page.total <= 1) {
        return;
    }
    const int max_start = page.total > kPageDotCount ? page.total - kPageDotCount : 0;
    const int start = LV_CLAMP(0, page.index - 1, max_start);
    for (int i = 0; i < kPageDotCount; ++i) {
        SetHidden(indicator.dots[i], i >= page.total);
        lv_obj_set_style_bg_opa(indicator.dots[i],
                                start + i == page.index ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    SetHidden(indicator.left, start == 0);
    SetHidden(indicator.right, start + kPageDotCount >= page.total);
}

void AppendCodepoint(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The built-in Montserrat fonts cover ASCII, the degree sign, the bullet and
// LVGL's symbol range. Anything else renders as an empty box, so calendar
// titles and Claude's replies get mapped onto what the font can draw.
std::string SanitizeText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (lead < 0x80) {
            len = 1;
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
            cp = lead & 0x07;
        } else {
            ++i;
            continue;
        }
        if (i + len > text.size()) {
            break;
        }
        for (size_t j = 1; j < len; ++j) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3F);
        }
        i += len;

        switch (cp) {
            case 0x2192:  // right arrows
            case 0x2794:
            case 0x279C:
            case 0x27A1:
            case 0x27A4:
            case 0x25B6:
                out += LV_SYMBOL_RIGHT;
                break;
            case 0x2190:  // left arrows
            case 0x2B05:
            case 0x25C0:
                out += LV_SYMBOL_LEFT;
                break;
            case 0x2013:
            case 0x2014:
            case 0x2212:
                out += '-';
                break;
            case 0x2018:
            case 0x2019:
                out += '\'';
                break;
            case 0x201C:
            case 0x201D:
                out += '"';
                break;
            case 0x2026:
                out += "...";
                break;
            case 0x00A0:
                out += ' ';
                break;
            default:
                if (cp < 0x80 || cp == 0xB0 || cp == 0x2022 || (cp >= 0xF000 && cp <= 0xF8FF)) {
                    AppendCodepoint(&out, cp);
                } else if (cp >= 0x80) {
                    out += '?';
                }
                break;
        }
    }
    return out;
}

void TickTimerCallback(void*) { lv_tick_inc(kTickPeriodMs); }

// LVGL drives drawing through its normal (color) renderer; we threshold
// each pixel's brightness into our 1bpp epaper framebuffer here rather
// than relying on LV_COLOR_DEPTH_1's internal packing. full_refresh=1
// guarantees this is called once per redraw covering the whole screen, so
// it's safe to trigger the real panel refresh at the end of every call.
void FlushCb(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
    for (lv_coord_t y = area->y1; y <= area->y2; ++y) {
        for (lv_coord_t x = area->x1; x <= area->x2; ++x) {
            const lv_color_t color = *color_p++;
            epaper_display::SetPixel(x, y, lv_color_brightness(color) < 128);
        }
    }
    const esp_err_t err = epaper_display::FullRefresh();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "FullRefresh failed: %s", esp_err_to_name(err));
    }
    lv_disp_flush_ready(disp_drv);
}

lv_obj_t* CreateScreen() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    return screen;
}

lv_obj_t* AddLabel(lv_obj_t* parent, const lv_font_t* font, lv_label_long_mode_t long_mode,
                   lv_coord_t width) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, long_mode);
    if (width > 0) {
        lv_obj_set_width(label, width);
    }
    return label;
}

// An object with none of the default theme's padding, border or background.
lv_obj_t* AddPlainObject(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

void BuildPageIndicator(lv_obj_t* screen, PageIndicator* indicator) {
    indicator->root = AddPlainObject(screen);
    lv_obj_set_size(indicator->root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(indicator->root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(indicator->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(indicator->root, 6, 0);
    lv_obj_set_flex_align(indicator->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    indicator->left = AddLabel(indicator->root, &lv_font_montserrat_14, LV_LABEL_LONG_CLIP, 0);
    lv_label_set_text(indicator->left, LV_SYMBOL_LEFT);
    for (lv_obj_t*& dot : indicator->dots) {
        dot = AddPlainObject(indicator->root);
        lv_obj_set_size(dot, kPageDotSize, kPageDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
    }
    indicator->right = AddLabel(indicator->root, &lv_font_montserrat_14, LV_LABEL_LONG_CLIP, 0);
    lv_label_set_text(indicator->right, LV_SYMBOL_RIGHT);

    lv_obj_align(indicator->root, LV_ALIGN_TOP_RIGHT, -kViewMargin, 28);
    SetHidden(indicator->root, true);
}

void BuildIdleScreen() {
    s_screen_idle = CreateScreen();

    lv_obj_t* title = AddLabel(s_screen_idle, &lv_font_montserrat_20, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(title, "sticky-claude");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    s_idle_status_label = AddLabel(s_screen_idle, &lv_font_montserrat_14, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(s_idle_status_label, "");
    lv_obj_align(s_idle_status_label, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t* prompt = AddLabel(s_screen_idle, &lv_font_montserrat_28, LV_LABEL_LONG_WRAP,
                                kContentWidth);
    lv_label_set_text(prompt, "Hold the button to talk");
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(prompt, LV_ALIGN_CENTER, 0, 0);

    BuildPageIndicator(s_screen_idle, &s_idle_indicator);
}

void BuildRecordingScreen() {
    s_screen_recording = CreateScreen();

    lv_obj_t* big = AddLabel(s_screen_recording, &lv_font_montserrat_28, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(big, "Listening...");
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -16);

    lv_obj_t* hint = AddLabel(s_screen_recording, &lv_font_montserrat_16, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(hint, "Release the button when you're done");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 32);
}

void BuildThinkingScreen() {
    s_screen_thinking = CreateScreen();

    lv_obj_t* big = AddLabel(s_screen_thinking, &lv_font_montserrat_28, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(big, "Thinking...");
    lv_obj_align(big, LV_ALIGN_CENTER, 0, 0);
}

void BuildReplyScreen() {
    s_screen_reply = CreateScreen();

    lv_obj_t* you_said_label = AddLabel(s_screen_reply, &lv_font_montserrat_14, LV_LABEL_LONG_WRAP,
                                       0);
    lv_label_set_text(you_said_label, "YOU SAID");
    lv_obj_align(you_said_label, LV_ALIGN_TOP_LEFT, kMargin, 24);

    s_reply_transcript_label = AddLabel(s_screen_reply, &lv_font_montserrat_16, LV_LABEL_LONG_WRAP,
                                        kContentWidth);
    lv_obj_align(s_reply_transcript_label, LV_ALIGN_TOP_LEFT, kMargin, 48);

    lv_obj_t* claude_label = AddLabel(s_screen_reply, &lv_font_montserrat_14, LV_LABEL_LONG_WRAP,
                                      0);
    lv_label_set_text(claude_label, "CLAUDE");
    lv_obj_align(claude_label, LV_ALIGN_TOP_LEFT, kMargin, 160);

    s_reply_body_label = AddLabel(s_screen_reply, &lv_font_montserrat_20, LV_LABEL_LONG_WRAP,
                                  kContentWidth);
    lv_obj_align(s_reply_body_label, LV_ALIGN_TOP_LEFT, kMargin, 184);

    BuildPageIndicator(s_screen_reply, &s_reply_indicator);
}

void BuildErrorScreen() {
    s_screen_error = CreateScreen();

    lv_obj_t* title = AddLabel(s_screen_error, &lv_font_montserrat_28, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(title, "Couldn't send that");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

    s_error_message_label = AddLabel(s_screen_error, &lv_font_montserrat_16, LV_LABEL_LONG_WRAP,
                                     kContentWidth);
    lv_obj_set_style_text_align(s_error_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_error_message_label, LV_ALIGN_CENTER, 0, 20);
}

void BuildLoadingScreen() {
    s_screen_loading = CreateScreen();

    lv_obj_t* big = AddLabel(s_screen_loading, &lv_font_montserrat_28, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(big, "Loading...");
    lv_obj_align(big, LV_ALIGN_CENTER, 0, 0);
}

void BuildDeviceViewScreen() {
    s_screen_device_view = CreateScreen();

    s_device_view_title = AddLabel(s_screen_device_view, &lv_font_montserrat_28, LV_LABEL_LONG_DOT,
                                   kViewWidth - kIndicatorWidth - 130);
    lv_obj_align(s_device_view_title, LV_ALIGN_TOP_LEFT, kViewMargin, 18);

    s_device_view_position = AddLabel(s_screen_device_view, &lv_font_montserrat_16,
                                      LV_LABEL_LONG_CLIP, 0);
    lv_obj_align(s_device_view_position, LV_ALIGN_TOP_RIGHT, -kViewMargin - kIndicatorWidth, 28);

    lv_obj_t* rule = AddPlainObject(s_screen_device_view);
    lv_obj_set_size(rule, kViewWidth, 2);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, kViewMargin, kViewHeaderHeight - 8);

    s_device_view_list = AddPlainObject(s_screen_device_view);
    lv_obj_add_flag(s_device_view_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_device_view_list, kViewWidth, kScreenHeight - kViewHeaderHeight - 8);
    lv_obj_align(s_device_view_list, LV_ALIGN_TOP_LEFT, kViewMargin, kViewHeaderHeight);
    lv_obj_set_scrollbar_mode(s_device_view_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_layout(s_device_view_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_device_view_list, LV_FLEX_FLOW_COLUMN);

    BuildPageIndicator(s_screen_device_view, &s_view_indicator);
}

void BuildDetailScreen() {
    s_screen_detail = CreateScreen();

    lv_obj_t* back = AddLabel(s_screen_detail, &lv_font_montserrat_20, LV_LABEL_LONG_CLIP, 0);
    lv_label_set_text(back, LV_SYMBOL_LEFT " Back");
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, kViewMargin, 24);

    lv_obj_t* rule = AddPlainObject(s_screen_detail);
    lv_obj_set_size(rule, kViewWidth, 2);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, kViewMargin, kViewHeaderHeight - 8);

    s_detail_area = AddPlainObject(s_screen_detail);
    lv_obj_add_flag(s_detail_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_detail_area, kViewWidth, kScreenHeight - kViewHeaderHeight - 8);
    lv_obj_align(s_detail_area, LV_ALIGN_TOP_LEFT, kViewMargin, kViewHeaderHeight);
    lv_obj_set_scrollbar_mode(s_detail_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(s_detail_area, 8, 0);
    lv_obj_set_style_pad_row(s_detail_area, 16, 0);
    lv_obj_set_layout(s_detail_area, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_detail_area, LV_FLEX_FLOW_COLUMN);

    s_detail_title = AddLabel(s_detail_area, &lv_font_montserrat_28, LV_LABEL_LONG_WRAP, 0);
    lv_obj_set_width(s_detail_title, lv_pct(100));
    s_detail_body = AddLabel(s_detail_area, &lv_font_montserrat_20, LV_LABEL_LONG_WRAP, 0);
    lv_obj_set_width(s_detail_body, lv_pct(100));

    BuildPageIndicator(s_screen_detail, &s_detail_indicator);
}

lv_obj_t* AddDeviceViewRow(const DeviceViewRow& row) {
    lv_obj_t* obj = AddPlainObject(s_device_view_list);
    lv_obj_set_width(obj, lv_pct(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(obj, kRowMinHeight, 0);
    lv_obj_set_style_pad_hor(obj, 14, 0);
    lv_obj_set_style_pad_ver(obj, 8, 0);
    lv_obj_set_style_pad_column(obj, 16, 0);
    lv_obj_set_layout(obj, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);

    if (row.checkable) {
        lv_obj_t* box = AddPlainObject(obj);
        lv_obj_set_size(box, kCheckboxSize, kCheckboxSize);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, lv_color_black(), 0);
        if (row.checked) {
            lv_obj_t* mark = AddLabel(box, &lv_font_montserrat_20, LV_LABEL_LONG_CLIP, 0);
            lv_label_set_text(mark, LV_SYMBOL_OK);
            lv_obj_center(mark);
        }
    }

    lv_obj_t* column = AddPlainObject(obj);
    lv_obj_set_flex_grow(column, 1);
    lv_obj_set_height(column, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(column, 2, 0);
    lv_obj_set_layout(column, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* primary = AddLabel(column, &lv_font_montserrat_20, LV_LABEL_LONG_WRAP, 0);
    lv_obj_set_width(primary, lv_pct(100));
    lv_label_set_text(primary, row.text.c_str());
    if (row.checked) {
        lv_obj_set_style_text_decor(primary, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    }

    if (!row.secondary.empty()) {
        lv_obj_t* secondary = AddLabel(column, &lv_font_montserrat_16, LV_LABEL_LONG_WRAP, 0);
        lv_obj_set_width(secondary, lv_pct(100));
        lv_label_set_text(secondary, row.secondary.c_str());
    }
    return obj;
}

// Publishes the on-screen position of each row for hit testing, and updates the
// header's count. Rows scrolled out of view get an empty band, which never
// matches. Runs on the UI task only.
void UpdateRowGeometry(size_t total_rows) {
    lv_area_t list_area;
    lv_obj_get_coords(s_device_view_list, &list_area);

    std::vector<RowBand> bands;
    bands.reserve(s_rendered_rows);
    int first_visible = -1;
    int last_visible = -1;
    for (size_t i = 0; i < s_rendered_rows; ++i) {
        lv_area_t area;
        lv_obj_get_coords(lv_obj_get_child(s_device_view_list, i), &area);
        bands.push_back({LV_MAX(area.y1, list_area.y1), LV_MIN(area.y2, list_area.y2)});
        if (area.y1 >= list_area.y1 && area.y2 <= list_area.y2) {
            if (first_visible < 0) {
                first_visible = static_cast<int>(i);
            }
            last_visible = static_cast<int>(i);
        }
    }

    char position[48] = "";
    if (first_visible == 0 && last_visible + 1 == static_cast<int>(total_rows)) {
        snprintf(position, sizeof(position), "%u item%s", static_cast<unsigned>(total_rows),
                 total_rows == 1 ? "" : "s");
    } else if (first_visible >= 0) {
        snprintf(position, sizeof(position), "%d-%d of %u", first_visible + 1, last_visible + 1,
                 static_cast<unsigned>(total_rows));
    }
    lv_label_set_text(s_device_view_position, position);
    lv_obj_align(s_device_view_position, LV_ALIGN_TOP_RIGHT, -kViewMargin - kIndicatorWidth, 28);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_row_bands = std::move(bands);
    xSemaphoreGive(s_lock);
}

// Runs on the UI task only.
void ShowScreen(lv_obj_t* screen) {
    if (screen != s_screen_device_view) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_device_view_shown = false;
        xSemaphoreGive(s_lock);
    }
    lv_scr_load(screen);
    lv_refr_now(nullptr);
}

// Runs on the UI task only.
void RenderDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows,
                      PagePos page, bool keep_scroll) {
    const lv_coord_t saved_scroll = keep_scroll ? lv_obj_get_scroll_y(s_device_view_list) : 0;

    lv_label_set_text(s_device_view_title, title.c_str());
    SetPageIndicator(s_view_indicator, page);
    lv_obj_clean(s_device_view_list);
    lv_obj_scroll_to_y(s_device_view_list, 0, LV_ANIM_OFF);

    s_rendered_rows = 0;
    lv_mem_monitor_t mem;
    for (const DeviceViewRow& row : rows) {
        lv_mem_monitor(&mem);
        if (mem.free_size < kMinLvglFreeForRow) {
            ESP_LOGW(kTag, "stopped at %u of %u rows: lvgl_free=%u",
                     static_cast<unsigned>(s_rendered_rows), static_cast<unsigned>(rows.size()),
                     static_cast<unsigned>(mem.free_size));
            break;
        }
        AddDeviceViewRow(row);
        ++s_rendered_rows;
    }

    if (rows.empty()) {
        lv_obj_t* empty = AddLabel(s_device_view_list, &lv_font_montserrat_20, LV_LABEL_LONG_WRAP,
                                   kViewWidth);
        lv_label_set_text(empty, "Nothing here yet.");
        lv_obj_set_style_pad_top(empty, 24, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_update_layout(s_screen_device_view);
    if (saved_scroll > 0) {
        const lv_coord_t max_scroll = LV_MAX(lv_obj_get_scroll_bottom(s_device_view_list), 0);
        lv_obj_scroll_to_y(s_device_view_list, LV_MIN(saved_scroll, max_scroll), LV_ANIM_OFF);
        lv_obj_update_layout(s_screen_device_view);
    }
    UpdateRowGeometry(s_rendered_rows);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_device_view_shown = true;
    xSemaphoreGive(s_lock);

    ShowScreen(s_screen_device_view);

    lv_mem_monitor(&mem);
    ESP_LOGI(kTag, "device view: rows=%u lvgl_free=%u", static_cast<unsigned>(s_rendered_rows),
             static_cast<unsigned>(mem.free_size));
}

// Where the list should move to, snapped so rows aren't cut off at the top:
// forward to the first row not fully visible, back by about a screen.
lv_coord_t NextListScroll(int direction) {
    const lv_coord_t current = lv_obj_get_scroll_y(s_device_view_list);
    const lv_coord_t height = lv_obj_get_content_height(s_device_view_list);
    const lv_coord_t limit = current + LV_MAX(lv_obj_get_scroll_bottom(s_device_view_list), 0);

    if (direction > 0) {
        for (size_t i = 0; i < s_rendered_rows; ++i) {
            lv_obj_t* row = lv_obj_get_child(s_device_view_list, i);
            const lv_coord_t top = lv_obj_get_y(row);
            if (top + lv_obj_get_height(row) > current + height) {
                // A row taller than the screen still has to make progress.
                return LV_MIN(top > current ? top : current + height, limit);
            }
        }
        return limit;
    }

    const lv_coord_t wanted = LV_MAX(current - height, 0);
    for (size_t i = 0; i < s_rendered_rows; ++i) {
        const lv_coord_t top = lv_obj_get_y(lv_obj_get_child(s_device_view_list, i));
        if (top >= wanted) {
            return top < current ? top : wanted;
        }
    }
    return wanted;
}

lv_coord_t NextDetailScroll(int direction) {
    const lv_coord_t current = lv_obj_get_scroll_y(s_detail_area);
    const lv_coord_t step = lv_obj_get_content_height(s_detail_area) - kPageOverlap;
    if (direction > 0) {
        return current + LV_MIN(step, LV_MAX(lv_obj_get_scroll_bottom(s_detail_area), 0));
    }
    return LV_MAX(current - step, 0);
}

// Runs on the UI task only.
void RenderScroll(int direction) {
    const lv_obj_t* screen = lv_scr_act();
    lv_obj_t* area = nullptr;
    lv_coord_t target = 0;
    if (screen == s_screen_device_view) {
        area = s_device_view_list;
        target = NextListScroll(direction);
    } else if (screen == s_screen_detail) {
        area = s_detail_area;
        target = NextDetailScroll(direction);
    } else {
        return;
    }
    if (target == lv_obj_get_scroll_y(area)) {
        return;  // already at this end, and a refresh would change nothing
    }
    lv_obj_scroll_to_y(area, target, LV_ANIM_OFF);
    lv_obj_update_layout(lv_scr_act());
    if (area == s_device_view_list) {
        UpdateRowGeometry(s_rendered_rows);
    }
    lv_refr_now(nullptr);
}

void UiTask(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            std::function<void()> work = std::move(s_pending);
            s_pending = nullptr;
            s_rendering = static_cast<bool>(work);
            xSemaphoreGive(s_lock);
            if (!work) {
                break;
            }
            work();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_rendering = false;
            xSemaphoreGive(s_lock);
        }
    }
}

void Post(std::function<void()> work) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending = std::move(work);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

}  // namespace

esp_err_t Init() {
    lv_init();

    const size_t buf_pixels = static_cast<size_t>(kScreenWidth) * kScreenHeight;
    auto* draw_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
    if (draw_buf == nullptr) {
        ESP_LOGE(kTag, "failed to allocate %u pixel draw buffer",
                 static_cast<unsigned>(buf_pixels));
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_draw_buf, draw_buf, nullptr, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = kScreenWidth;
    s_disp_drv.ver_res = kScreenHeight;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = &FlushCb;
    s_disp_drv.full_refresh = 1;  // e-paper only supports full-screen refresh here
    lv_disp_drv_register(&s_disp_drv);

    const esp_timer_create_args_t timer_args = {
        .callback = &TickTimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lv_tick",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_tick_timer);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_timer_start_periodic(s_tick_timer, kTickPeriodMs * 1000ULL);
    if (err != ESP_OK) {
        return err;
    }

    BuildIdleScreen();
    BuildRecordingScreen();
    BuildThinkingScreen();
    BuildReplyScreen();
    BuildErrorScreen();
    BuildLoadingScreen();
    BuildDeviceViewScreen();
    BuildDetailScreen();

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(UiTask, "ui", kUiTaskStackBytes, nullptr, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "initialized: %dx%d", kScreenWidth, kScreenHeight);
    return ESP_OK;
}

void ShowIdle(bool wifi_connected, int total_pages) {
    Post([wifi_connected, total_pages] {
        // A needless two-second refresh would be very visible on e-paper.
        const int pages = total_pages > 1 ? total_pages : 1;
        if (lv_scr_act() == s_screen_idle && s_idle_wifi == wifi_connected &&
            s_idle_pages == pages) {
            return;
        }
        s_idle_wifi = wifi_connected;
        s_idle_pages = pages;
        lv_label_set_text(s_idle_status_label,
                          wifi_connected ? "Connected" : "No WiFi -- uploads will fail");
        SetPageIndicator(s_idle_indicator, {0, pages});
        ShowScreen(s_screen_idle);
    });
}

void ShowRecording() {
    Post([] { ShowScreen(s_screen_recording); });
}

void ShowThinking() {
    Post([] { ShowScreen(s_screen_thinking); });
}

void ShowReply(const std::string& transcript, const std::string& reply, int total_pages) {
    Post([transcript = SanitizeText(transcript), reply = SanitizeText(reply), total_pages] {
        lv_label_set_text(s_reply_transcript_label, transcript.c_str());
        lv_label_set_text(s_reply_body_label, reply.c_str());
        SetPageIndicator(s_reply_indicator, {0, total_pages});
        ShowScreen(s_screen_reply);
    });
}

void ShowError(const std::string& message) {
    Post([message = SanitizeText(message)] {
        lv_label_set_text(s_error_message_label, message.c_str());
        ShowScreen(s_screen_error);
    });
}

void ShowLoading() {
    Post([] { ShowScreen(s_screen_loading); });
}

void ShowDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows,
                    PagePos page, bool keep_scroll) {
    std::vector<DeviceViewRow> clean = rows;
    for (DeviceViewRow& row : clean) {
        row.text = SanitizeText(row.text);
        row.secondary = SanitizeText(row.secondary);
    }
    Post([title = SanitizeText(title), clean = std::move(clean), page, keep_scroll] {
        RenderDeviceView(title, clean, page, keep_scroll);
    });
}

void ShowDetail(const std::string& title, const std::string& body, PagePos page) {
    Post([title = SanitizeText(title), body = SanitizeText(body), page] {
        lv_label_set_text(s_detail_title, title.c_str());
        lv_label_set_text(s_detail_body, body.c_str());
        SetPageIndicator(s_detail_indicator, page);
        lv_obj_update_layout(s_screen_detail);
        lv_obj_scroll_to_y(s_detail_area, 0, LV_ANIM_OFF);
        ShowScreen(s_screen_detail);
    });
}

void Scroll(int direction) {
    if (IsBusy()) {
        return;
    }
    Post([direction] { RenderScroll(direction); });
}

bool IsBusy() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_rendering || static_cast<bool>(s_pending);
    xSemaphoreGive(s_lock);
    return busy;
}

int HitTestDeviceViewRow(int x, int y) {
    int hit = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_device_view_shown && !s_rendering && !s_pending && x >= 0 && x < kScreenWidth) {
        for (size_t i = 0; i < s_row_bands.size(); ++i) {
            if (y >= s_row_bands[i].y1 && y <= s_row_bands[i].y2) {
                hit = static_cast<int>(i);
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return hit;
}

}  // namespace ui
