#include "device_nav.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings_service.h"
#include "ui.h"

namespace device_nav {
namespace {

constexpr const char* kTag = "device_nav";
constexpr int kFetchTimeoutMs = 15000;  // a CalDAV lookup behind this can take several seconds
constexpr int kActionTimeoutMs = 5000;
constexpr int64_t kRefreshMinIntervalUs = 30 * 1000 * 1000;  // for page changes in quick succession
constexpr uint32_t kTaskStackBytes = 6144;
constexpr size_t kMaxIdLen = 40;
constexpr size_t kQueueLength = 8;

struct Item {
    std::string id;
    std::string text;
    std::string secondary;
    std::string detail;
    bool checkable = false;
    bool checked = false;

    bool operator==(const Item&) const = default;
};

struct View {
    std::string plugin_id;
    std::string view_type;
    std::string title;
    std::string body;
    std::vector<Item> items;

    bool operator==(const View&) const = default;
};

enum class CommandType { kRefresh, kPostAction };

struct Command {
    CommandType type;
    bool checked = false;
    char plugin_id[kMaxIdLen] = {};
    char item_id[kMaxIdLen] = {};
};

QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;

// s_state_mutex guards all of this. Buttons, touch and DeviceNavTask all change it.
SemaphoreHandle_t s_state_mutex = nullptr;
std::vector<View> s_views;
bool s_has_fetched = false;  // distinguishes "fetched, turned out empty" from "never fetched"
int s_view_index = -1;       // -1 = home
int s_detail_row = -1;       // row whose details are open, -1 = none
bool s_wifi_connected = false;
bool s_input_paused = false;
bool s_home_has_message = false;  // a reply or error is on screen at home
bool s_refresh_in_flight = false;
int s_pending_nav = 0;        // page change waiting on the first fetch
bool s_force_render = false;  // the next fetch has to draw, whatever it returns
int s_posts_pending = 0;
int64_t s_last_fetch_us = 0;  // 0 = the cache needs refreshing at the next chance

void Lock() { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
void Unlock() { xSemaphoreGive(s_state_mutex); }

void CopyBounded(char* dest, size_t dest_len, const std::string& src) {
    strncpy(dest, src.c_str(), dest_len - 1);
    dest[dest_len - 1] = '\0';
}

std::string BaseUrl() {
    // settings_service's backend_url is the full /converse endpoint
    // (e.g. "http://192.168.1.50:8000/converse"), typed into the captive
    // portal; every other endpoint lives on that same host/port.
    const std::string converse_url = settings_service::Get().backend_url;
    const size_t scheme_end = converse_url.find("://");
    if (scheme_end == std::string::npos) {
        return converse_url;
    }
    const size_t host_end = converse_url.find('/', scheme_end + 3);
    return host_end == std::string::npos ? converse_url : converse_url.substr(0, host_end);
}

struct ResponseBuffer {
    std::string body;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr) {
        auto* buffer = static_cast<ResponseBuffer*>(event->user_data);
        buffer->body.append(static_cast<const char*>(event->data),
                            static_cast<size_t>(event->data_len));
    }
    return ESP_OK;
}

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(field) ? field->valuestring : "";
}

// Fetches and parses GET /api/device/views. Only ever called from
// DeviceNavTask -- this is a blocking network call.
bool FetchViews(std::vector<View>* out_views) {
    const std::string url = BaseUrl() + "/api/device/views";
    ResponseBuffer response_buffer;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kFetchTimeoutMs;
    config.event_handler = &HttpEventHandler;
    config.user_data = &response_buffer;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(kTag, "http client init failed");
        return false;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(kTag, "GET /api/device/views failed: err=%s http=%d", esp_err_to_name(err),
                 status);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(response_buffer.body.c_str(), response_buffer.body.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "invalid JSON from /api/device/views");
        return false;
    }

    std::vector<View> views;
    cJSON* views_json = cJSON_GetObjectItemCaseSensitive(root, "views");
    cJSON* view_json = nullptr;
    cJSON_ArrayForEach(view_json, views_json) {
        View view;
        view.plugin_id = JsonString(view_json, "plugin_id");
        view.view_type = JsonString(view_json, "view_type");
        view.title = JsonString(view_json, "title");
        view.body = JsonString(view_json, "body");

        cJSON* items_json = cJSON_GetObjectItemCaseSensitive(view_json, "items");
        cJSON* item_json = nullptr;
        cJSON_ArrayForEach(item_json, items_json) {
            Item item;
            item.id = JsonString(item_json, "id");
            item.text = JsonString(item_json, "text");
            item.secondary = JsonString(item_json, "secondary");
            item.detail = JsonString(item_json, "detail");
            const cJSON* checked = cJSON_GetObjectItemCaseSensitive(item_json, "checked");
            item.checkable = cJSON_IsBool(checked);
            item.checked = item.checkable && cJSON_IsTrue(checked);
            view.items.push_back(std::move(item));
        }
        views.push_back(std::move(view));
    }
    cJSON_Delete(root);

    *out_views = std::move(views);
    return true;
}

// Sends the state the row should now have, rather than asking for a toggle, so
// a repeated request can't flip it back. Only ever called from DeviceNavTask.
bool PostDeviceAction(const char* plugin_id, const char* item_id, bool checked) {
    cJSON* json = cJSON_CreateObject();
    if (json == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(json, "item_id", item_id);
    cJSON_AddBoolToObject(json, "checked", checked);
    char* body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == nullptr) {
        return false;
    }

    const std::string url = BaseUrl() + "/api/plugins/" + plugin_id + "/device-action";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kActionTimeoutMs;

    bool ok = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
        const esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        ok = err == ESP_OK && status >= 200 && status < 300;
        if (!ok) {
            ESP_LOGW(kTag, "POST device-action failed: plugin=%s item=%s err=%s http=%d", plugin_id,
                     item_id, esp_err_to_name(err), status);
        }
    }
    cJSON_free(body);
    return ok;
}

// Caller must hold the lock, as for every *Locked function here.
void RenderCurrentLocked(bool keep_scroll) {
    const int total = static_cast<int>(s_views.size()) + 1;
    if (s_view_index < 0 || s_view_index >= static_cast<int>(s_views.size())) {
        ui::ShowIdle(s_wifi_connected, s_has_fetched ? total : 0);
        return;
    }
    const View& view = s_views[s_view_index];
    const ui::PagePos page = {s_view_index + 1, total};
    if (s_detail_row >= 0 && s_detail_row < static_cast<int>(view.items.size())) {
        const Item& item = view.items[s_detail_row];
        ui::ShowDetail(item.text, item.detail, page);
        return;
    }
    std::vector<ui::DeviceViewRow> rows;
    rows.reserve(view.items.size());
    for (const Item& item : view.items) {
        ui::DeviceViewRow row;
        row.text = item.text;
        row.secondary = item.secondary;
        row.checkable = item.checkable;
        row.checked = item.checked;
        rows.push_back(std::move(row));
    }
    ui::ShowDeviceView(view.title, rows, page, keep_scroll);
}

void EnqueueRefreshLocked() {
    if (s_refresh_in_flight) {
        return;  // already fetching; let that one land rather than piling up
    }
    Command command = {};
    command.type = CommandType::kRefresh;
    if (xQueueSend(s_queue, &command, 0) == pdTRUE) {
        s_refresh_in_flight = true;
    } else {
        ESP_LOGW(kTag, "queue full, refresh dropped");
    }
}

void MovePageLocked(int direction) {
    const int total = static_cast<int>(s_views.size()) + 1;
    const int page = (s_view_index + 1 + direction + total) % total;
    s_view_index = page - 1;
    s_detail_row = -1;
}

// Moves a toggled row to where it belongs: done rows at the bottom, in the order
// they were finished, and reopened rows just above them.
void ToggleRowLocked(View* view, int row) {
    Item item = std::move(view->items[row]);
    view->items.erase(view->items.begin() + row);
    item.checked = !item.checked;
    auto position = view->items.end();
    if (!item.checked) {
        position = std::find_if(view->items.begin(), view->items.end(),
                                [](const Item& other) { return other.checked; });
    }
    view->items.insert(position, std::move(item));
}

// Swaps in freshly fetched views, keeping the current plugin's page if it still
// exists. Returns whether anything on the current screen changed.
bool ApplyViewsLocked(std::vector<View> views) {
    int new_index = -1;
    bool current_changed = false;
    if (s_view_index >= 0 && s_view_index < static_cast<int>(s_views.size())) {
        const View& current = s_views[s_view_index];
        for (size_t i = 0; i < views.size(); ++i) {
            if (views[i].plugin_id == current.plugin_id) {
                new_index = static_cast<int>(i);
                break;
            }
        }
        current_changed = new_index < 0 || !(views[new_index] == current);
    }
    const bool count_changed = !s_has_fetched || views.size() != s_views.size();

    s_views = std::move(views);
    s_has_fetched = true;
    s_view_index = new_index;
    if (current_changed) {
        s_detail_row = -1;  // the row it was showing may no longer exist
    }
    return current_changed || count_changed;
}

void HandleRefresh() {
    std::vector<View> views;
    const bool ok = FetchViews(&views);

    Lock();
    s_refresh_in_flight = false;
    bool render = s_force_render;
    s_force_render = false;
    const int pending_nav = s_pending_nav;
    s_pending_nav = 0;

    // A toggle that hasn't reached the backend yet would be undone by this
    // result, which predates it. The next page change refreshes again.
    if (ok && s_posts_pending == 0) {
        s_last_fetch_us = esp_timer_get_time();
        if (ApplyViewsLocked(std::move(views))) {
            render = true;
        }
    }
    bool keep_scroll = true;
    if (pending_nav != 0) {
        MovePageLocked(pending_nav);
        keep_scroll = false;
        render = true;
    }
    const bool covering_message = s_view_index < 0 && s_home_has_message;
    if (render && !s_input_paused && !covering_message) {
        RenderCurrentLocked(keep_scroll);
    }
    Unlock();
}

void HandlePostAction(const Command& command) {
    const bool ok = PostDeviceAction(command.plugin_id, command.item_id, command.checked);
    Lock();
    --s_posts_pending;
    if (!ok) {
        EnqueueRefreshLocked();  // put the row back to what the backend really has
    }
    Unlock();
}

void DeviceNavTask(void*) {
    Command command;
    while (true) {
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (command.type) {
            case CommandType::kRefresh:
                HandleRefresh();
                break;
            case CommandType::kPostAction:
                HandlePostAction(command);
                break;
        }
    }
}

}  // namespace

esp_err_t Init() {
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreate(kQueueLength, sizeof(Command));
    if (s_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t created =
        xTaskCreate(DeviceNavTask, "device_nav", kTaskStackBytes, nullptr, 4, &s_task);
    if (created != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "initialized");
    return ESP_OK;
}

void SetWifiConnected(bool connected) {
    Lock();
    s_wifi_connected = connected;
    Unlock();
}

void Preload() {
    Lock();
    s_force_render = true;
    EnqueueRefreshLocked();
    Unlock();
}

int KnownPluginViewCount() {
    Lock();
    const int count = s_has_fetched ? static_cast<int>(s_views.size()) : -1;
    Unlock();
    return count;
}

void PauseInput() {
    Lock();
    s_input_paused = true;
    s_pending_nav = 0;
    s_force_render = false;
    Unlock();
}

void ResumeInput() {
    Lock();
    s_input_paused = false;
    Unlock();
}

void ResetToHome() {
    Lock();
    s_view_index = -1;
    s_detail_row = -1;
    Unlock();
}

void FinishConversation() {
    Lock();
    s_input_paused = false;
    s_home_has_message = true;
    s_view_index = -1;
    s_detail_row = -1;
    EnqueueRefreshLocked();
    Unlock();
}

void Navigate(int direction) {
    Lock();
    if (s_input_paused) {
        Unlock();
        return;
    }
    s_home_has_message = false;

    if (!s_has_fetched) {
        if (s_pending_nav == 0) {
            ui::ShowLoading();
        }
        s_pending_nav = direction;
        s_force_render = true;  // Loading has to be replaced even if the fetch fails
        EnqueueRefreshLocked();
        Unlock();
        return;
    }

    MovePageLocked(direction);
    RenderCurrentLocked(/*keep_scroll=*/false);
    if (esp_timer_get_time() - s_last_fetch_us >= kRefreshMinIntervalUs) {
        EnqueueRefreshLocked();
    }
    Unlock();
}

void OnScroll(int direction) {
    Lock();
    if (!s_input_paused && s_view_index >= 0) {
        ui::Scroll(direction);
    }
    Unlock();
}

void OnTap(int x, int y) {
    Lock();
    if (s_input_paused || s_view_index < 0 || ui::IsBusy()) {
        Unlock();
        return;
    }
    if (s_detail_row >= 0) {
        s_detail_row = -1;
        RenderCurrentLocked(/*keep_scroll=*/true);
        Unlock();
        return;
    }

    View& view = s_views[s_view_index];
    const int row = ui::HitTestDeviceViewRow(x, y);
    if (row < 0 || row >= static_cast<int>(view.items.size())) {
        Unlock();
        return;
    }

    const Item& item = view.items[row];
    if (item.checkable) {
        Command command = {};
        command.type = CommandType::kPostAction;
        command.checked = !item.checked;
        CopyBounded(command.plugin_id, sizeof(command.plugin_id), view.plugin_id);
        CopyBounded(command.item_id, sizeof(command.item_id), item.id);
        if (xQueueSend(s_queue, &command, 0) == pdTRUE) {
            ++s_posts_pending;
            ToggleRowLocked(&view, row);
            RenderCurrentLocked(/*keep_scroll=*/true);
        } else {
            ESP_LOGW(kTag, "queue full, tap dropped");
        }
    } else if (!item.detail.empty()) {
        s_detail_row = row;
        RenderCurrentLocked(/*keep_scroll=*/true);
    }
    Unlock();
}

void OnBack() {
    Lock();
    if (s_detail_row >= 0) {
        s_detail_row = -1;
        RenderCurrentLocked(/*keep_scroll=*/true);
    } else {
        s_view_index = -1;
        s_home_has_message = false;
        RenderCurrentLocked(/*keep_scroll=*/false);
    }
    Unlock();
}

}  // namespace device_nav
