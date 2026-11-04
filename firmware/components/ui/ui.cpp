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

lv_obj_t* s_idle_status_label = nullptr;
lv_obj_t* s_reply_transcript_label = nullptr;
lv_obj_t* s_reply_body_label = nullptr;
lv_obj_t* s_error_message_label = nullptr;

void TickTimerCallback(void*) { lv_tick_inc(kTickPeriodMs); }

// LVGL drives drawing through its normal (color) renderer; we threshold
// each pixel's brightness into our 1bpp epaper framebuffer here rather
// than relying on LV_COLOR_DEPTH_1's internal packing, whose exact layout
// we didn't want to gamble on getting right without hardware to iterate
// against. disp_drv.full_refresh=1 guarantees this is called once per
// redraw covering the whole screen, so it's safe to trigger the real
// panel refresh at the end of every call.
void FlushCb(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
    for (lv_coord_t y = area->y1; y <= area->y2; ++y) {
        for (lv_coord_t x = area->x1; x <= area->x2; ++x) {
            const lv_color_t color = *color_p++;
            const bool black = lv_color_brightness(color) < 128;
            epaper_display::SetPixel(x, y, black);
        }
    }

    epaper_display::FullRefresh();
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

void ShowScreen(lv_obj_t* screen) {
    lv_scr_load(screen);
    lv_refr_now(nullptr);
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

    ESP_LOGI(kTag, "initialized: %dx%d", kScreenWidth, kScreenHeight);
    return ESP_OK;
}

void ShowIdle(bool wifi_connected) {
    lv_label_set_text(s_idle_status_label, wifi_connected ? "Connected" : "No WiFi -- uploads will fail");
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

}  // namespace ui
