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

lv_obj_t* s_idle_status_label = nullptr;
lv_obj_t* s_reply_transcript_label = nullptr;
lv_obj_t* s_reply_body_label = nullptr;
lv_obj_t* s_error_message_label = nullptr;
lv_obj_t* s_device_view_title = nullptr;
lv_obj_t* s_device_view_position = nullptr;
lv_obj_t* s_device_view_list = nullptr;

bool s_idle_wifi = false;
int s_idle_pages = -1;

// Page-position indicator on the idle screen: up to 3 dots (home plus the
// next couple of plugin views) with an arrow if there are more beyond that.
constexpr int kPageDotCount = 3;
constexpr lv_coord_t kPageDotSize = 10;
lv_obj_t* s_page_indicator = nullptr;
lv_obj_t* s_page_dots[kPageDotCount] = {nullptr, nullptr, nullptr};
lv_obj_t* s_page_arrow = nullptr;

void SetDotFilled(lv_obj_t* dot, bool filled) {
    lv_obj_set_style_bg_opa(dot, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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

    s_page_indicator = lv_obj_create(s_screen_idle);
    lv_obj_set_size(s_page_indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(s_page_indicator, 0, 0);
    lv_obj_set_style_pad_all(s_page_indicator, 0, 0);
    lv_obj_set_style_bg_opa(s_page_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_layout(s_page_indicator, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_page_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_page_indicator, 6, 0);
    lv_obj_set_flex_align(s_page_indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_page_indicator, LV_ALIGN_BOTTOM_RIGHT, -20, -20);

    for (lv_obj_t*& dot : s_page_dots) {
        dot = lv_obj_create(s_page_indicator);
        lv_obj_set_size(dot, kPageDotSize, kPageDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_border_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }

    s_page_arrow = AddLabel(s_page_indicator, &lv_font_montserrat_14, LV_LABEL_LONG_WRAP, 0);
    lv_label_set_text(s_page_arrow, LV_SYMBOL_RIGHT);
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
                                   kViewWidth - 120);
    lv_obj_align(s_device_view_title, LV_ALIGN_TOP_LEFT, kViewMargin, 18);

    s_device_view_position = AddLabel(s_screen_device_view, &lv_font_montserrat_16,
                                      LV_LABEL_LONG_CLIP, 0);
    lv_obj_align(s_device_view_position, LV_ALIGN_TOP_RIGHT, -kViewMargin, 28);

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
}

lv_obj_t* AddDeviceViewRow(const DeviceViewRow& row) {
    const lv_color_t ink = row.highlighted ? lv_color_white() : lv_color_black();

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
    if (row.highlighted) {
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    }

    if (row.checkable) {
        lv_obj_t* box = AddPlainObject(obj);
        lv_obj_set_size(box, kCheckboxSize, kCheckboxSize);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, ink, 0);
        if (row.checked) {
            lv_obj_t* mark = AddLabel(box, &lv_font_montserrat_20, LV_LABEL_LONG_CLIP, 0);
            lv_label_set_text(mark, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(mark, ink, 0);
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
    lv_obj_set_style_text_color(primary, ink, 0);

    if (!row.secondary.empty()) {
        lv_obj_t* secondary = AddLabel(column, &lv_font_montserrat_16, LV_LABEL_LONG_WRAP, 0);
        lv_obj_set_width(secondary, lv_pct(100));
        lv_label_set_text(secondary, row.secondary.c_str());
        lv_obj_set_style_text_color(secondary, ink, 0);
    }
    return obj;
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
void RenderDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows) {
    lv_label_set_text(s_device_view_title, title.c_str());
    lv_obj_clean(s_device_view_list);
    lv_obj_scroll_to_y(s_device_view_list, 0, LV_ANIM_OFF);

    std::vector<lv_obj_t*> row_objs;
    row_objs.reserve(rows.size());
    lv_obj_t* highlighted = nullptr;
    int highlighted_index = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        lv_obj_t* obj = AddDeviceViewRow(rows[i]);
        row_objs.push_back(obj);
        if (rows[i].highlighted) {
            highlighted = obj;
            highlighted_index = static_cast<int>(i);
        }
    }

    if (rows.empty()) {
        lv_obj_t* empty = AddLabel(s_device_view_list, &lv_font_montserrat_20, LV_LABEL_LONG_WRAP,
                                   kViewWidth);
        lv_label_set_text(empty, "Nothing here yet.");
        lv_obj_set_style_pad_top(empty, 24, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_device_view_position, "");
    } else {
        char position[24];
        snprintf(position, sizeof(position), "%d / %u",
                 highlighted_index >= 0 ? highlighted_index + 1 : 1,
                 static_cast<unsigned>(rows.size()));
        lv_label_set_text(s_device_view_position, position);
    }
    lv_obj_align(s_device_view_position, LV_ALIGN_TOP_RIGHT, -kViewMargin, 28);

    lv_obj_update_layout(s_screen_device_view);
    if (highlighted != nullptr) {
        lv_obj_scroll_to_view(highlighted, LV_ANIM_OFF);
        lv_obj_update_layout(s_screen_device_view);
    }

    lv_area_t list_area;
    lv_obj_get_coords(s_device_view_list, &list_area);
    std::vector<RowBand> bands;
    bands.reserve(row_objs.size());
    for (lv_obj_t* obj : row_objs) {
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        bands.push_back({LV_MAX(area.y1, list_area.y1), LV_MIN(area.y2, list_area.y2)});
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_row_bands = std::move(bands);
    s_device_view_shown = true;
    xSemaphoreGive(s_lock);

    ShowScreen(s_screen_device_view);

    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    ESP_LOGI(kTag, "device view: rows=%u lvgl_free=%u", static_cast<unsigned>(rows.size()),
             static_cast<unsigned>(mem.free_size));
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
        if (lv_scr_act() == s_screen_idle && s_idle_wifi == wifi_connected &&
            s_idle_pages == total_pages) {
            return;
        }
        s_idle_wifi = wifi_connected;
        s_idle_pages = total_pages;
        lv_label_set_text(s_idle_status_label,
                          wifi_connected ? "Connected" : "No WiFi -- uploads will fail");

        if (total_pages <= 1) {
            lv_obj_add_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
            const int dot_count = total_pages < kPageDotCount ? total_pages : kPageDotCount;
            for (int i = 0; i < kPageDotCount; ++i) {
                if (i < dot_count) {
                    lv_obj_clear_flag(s_page_dots[i], LV_OBJ_FLAG_HIDDEN);
                    SetDotFilled(s_page_dots[i], i == 0);  // home is always first and current
                } else {
                    lv_obj_add_flag(s_page_dots[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (total_pages > kPageDotCount) {
                lv_obj_clear_flag(s_page_arrow, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_page_arrow, LV_OBJ_FLAG_HIDDEN);
            }
        }
        ShowScreen(s_screen_idle);
    });
}

void ShowRecording() {
    Post([] { ShowScreen(s_screen_recording); });
}

void ShowThinking() {
    Post([] { ShowScreen(s_screen_thinking); });
}

void ShowReply(const std::string& transcript, const std::string& reply) {
    Post([transcript = SanitizeText(transcript), reply = SanitizeText(reply)] {
        lv_label_set_text(s_reply_transcript_label, transcript.c_str());
        lv_label_set_text(s_reply_body_label, reply.c_str());
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

void ShowDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows) {
    std::vector<DeviceViewRow> clean = rows;
    for (DeviceViewRow& row : clean) {
        row.text = SanitizeText(row.text);
        row.secondary = SanitizeText(row.secondary);
    }
    Post([title = SanitizeText(title), clean = std::move(clean)] { RenderDeviceView(title, clean); });
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
