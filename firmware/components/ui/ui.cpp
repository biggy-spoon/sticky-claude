#include "ui.h"

#include "board_config.h"
#include "epaper_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace ui {
namespace {

constexpr const char* kTag = "ui";
constexpr uint32_t kTickPeriodMs = 5;
constexpr lv_coord_t kScreenWidth = BOARD_EPD_WIDTH;
constexpr lv_coord_t kScreenHeight = BOARD_EPD_HEIGHT;
constexpr lv_coord_t kMargin = 48;
constexpr lv_coord_t kContentWidth = kScreenWidth - 2 * kMargin;

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
esp_timer_handle_t s_tick_timer = nullptr;

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
lv_obj_t* s_device_view_list = nullptr;

// Page-position indicator on the idle screen: up to 3 dots (home plus the
// next couple of plugin views) with an arrow if there are more beyond
// that. Home is always shown first/filled -- see ShowIdle().
constexpr int kPageDotCount = 3;
constexpr lv_coord_t kPageDotSize = 10;
lv_obj_t* s_page_indicator = nullptr;
lv_obj_t* s_page_dots[kPageDotCount] = {nullptr, nullptr, nullptr};
lv_obj_t* s_page_arrow = nullptr;

void SetDotFilled(lv_obj_t* dot, bool filled) {
    lv_obj_set_style_bg_opa(dot, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void TickTimerCallback(void*) { lv_tick_inc(kTickPeriodMs); }

// LVGL drives drawing through its normal (color) renderer; we threshold
// each pixel's brightness into our 1bpp epaper framebuffer here rather
// than relying on LV_COLOR_DEPTH_1's internal packing, whose exact layout
// we didn't want to gamble on getting right without hardware to iterate
// against. disp_drv.full_refresh=1 guarantees this is called once per
// redraw covering the whole screen, so it's safe to trigger the real
// panel refresh at the end of every call.
void FlushCb(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t black_count = 0;
    uint32_t pixel_count = 0;
    for (lv_coord_t y = area->y1; y <= area->y2; ++y) {
        for (lv_coord_t x = area->x1; x <= area->x2; ++x) {
            const lv_color_t color = *color_p++;
            const bool black = lv_color_brightness(color) < 128;
            epaper_display::SetPixel(x, y, black);
            ++pixel_count;
            if (black) {
                ++black_count;
            }
        }
    }
    ESP_LOGI(kTag, "flush: area=(%d,%d)-(%d,%d) pixels=%lu black=%lu", area->x1, area->y1,
             area->x2, area->y2, static_cast<unsigned long>(pixel_count),
             static_cast<unsigned long>(black_count));

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
    lv_label_set_text(s_page_arrow, ">");
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

    s_device_view_title = AddLabel(s_screen_device_view, &lv_font_montserrat_20, LV_LABEL_LONG_WRAP,
                                   kContentWidth);
    lv_obj_align(s_device_view_title, LV_ALIGN_TOP_MID, 0, 20);

    s_device_view_list = lv_obj_create(s_screen_device_view);
    lv_obj_set_size(s_device_view_list, kContentWidth, kScreenHeight - 80);
    lv_obj_align(s_device_view_list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_border_width(s_device_view_list, 0, 0);
    lv_obj_set_style_pad_all(s_device_view_list, 0, 0);
    lv_obj_set_style_bg_opa(s_device_view_list, LV_OPA_TRANSP, 0);
    lv_obj_set_layout(s_device_view_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_device_view_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_device_view_list, 10, 0);
    lv_obj_clear_flag(s_device_view_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
}

void ShowScreen(lv_obj_t* screen) {
    ESP_LOGI(kTag, "ShowScreen: loading %p", screen);
    lv_scr_load(screen);
    lv_refr_now(nullptr);
    ESP_LOGI(kTag, "ShowScreen: refresh done");
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

    ESP_LOGI(kTag, "initialized: %dx%d", kScreenWidth, kScreenHeight);
    return ESP_OK;
}

void ShowIdle(bool wifi_connected, int total_pages) {
    lv_label_set_text(s_idle_status_label, wifi_connected ? "Connected" : "No WiFi -- uploads will fail");

    if (total_pages <= 1) {
        lv_obj_add_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_page_indicator, LV_OBJ_FLAG_HIDDEN);
        const int dot_count = total_pages < kPageDotCount ? total_pages : kPageDotCount;
        for (int i = 0; i < kPageDotCount; ++i) {
            if (i < dot_count) {
                lv_obj_clear_flag(s_page_dots[i], LV_OBJ_FLAG_HIDDEN);
                SetDotFilled(s_page_dots[i], i == 0);  // home is always shown first and current
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
}

void ShowRecording() { ShowScreen(s_screen_recording); }

void ShowThinking() { ShowScreen(s_screen_thinking); }

void ShowReply(const std::string& transcript, const std::string& reply) {
    lv_label_set_text(s_reply_transcript_label, transcript.c_str());
    lv_label_set_text(s_reply_body_label, reply.c_str());
    ShowScreen(s_screen_reply);
}

void ShowError(const std::string& message) {
    lv_label_set_text(s_error_message_label, message.c_str());
    ShowScreen(s_screen_error);
}

void ShowLoading() { ShowScreen(s_screen_loading); }

void ShowDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows) {
    lv_label_set_text(s_device_view_title, title.c_str());
    lv_obj_clean(s_device_view_list);

    if (rows.empty()) {
        lv_obj_t* empty = AddLabel(s_device_view_list, &lv_font_montserrat_16, LV_LABEL_LONG_WRAP,
                                   kContentWidth);
        lv_label_set_text(empty, "Nothing here yet.");
        ShowScreen(s_screen_device_view);
        return;
    }

    for (const DeviceViewRow& row : rows) {
        std::string line = row.checkable ? (row.checked ? "[x] " : "[ ] ") : "- ";
        line += row.text;

        lv_obj_t* label = AddLabel(s_device_view_list, &lv_font_montserrat_16, LV_LABEL_LONG_WRAP,
                                   kContentWidth);
        lv_label_set_text(label, line.c_str());
        lv_obj_set_style_pad_ver(label, 4, 0);
        if (row.highlighted) {
            lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(label, lv_color_black(), 0);
            lv_obj_set_style_text_color(label, lv_color_white(), 0);
        }
    }
    ShowScreen(s_screen_device_view);
}

}  // namespace ui
