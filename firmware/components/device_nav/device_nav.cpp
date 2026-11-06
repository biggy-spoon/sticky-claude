#include "device_nav.h"

#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings_service.h"
#include "ui.h"

namespace device_nav {
namespace {

constexpr const char* kTag = "device_nav";
constexpr int kHttpTimeoutMs = 5000;  // local LAN call, not the /converse STT+Claude round trip
constexpr uint32_t kTaskStackWords = 6144;
constexpr size_t kMaxIdLen = 40;
constexpr size_t kQueueLength = 8;

struct Item {
    std::string id;
    std::string text;
    std::string secondary;
    bool checkable = false;
    bool checked = false;
};

struct View {
    std::string plugin_id;
    std::string view_type;
    std::string title;
    std::string body;
    std::vector<Item> items;
};

enum class CommandType { kRefresh, kPostAction };

struct Command {
    CommandType type;
    bool enter_from_end = false;  // kRefresh only: true = arriving via Up, false = via Down
    char plugin_id[kMaxIdLen] = {};
    char item_id[kMaxIdLen] = {};
};

QueueHandle_t s_queue = nullptr;
SemaphoreHandle_t s_state_mutex = nullptr;
TaskHandle_t s_task = nullptr;
bool s_refresh_in_flight = false;

// Protected by s_state_mutex. Buttons, touch and DeviceNavTask all change it.
int s_view_index = -1;  // -1 = home/conversation screen
int s_cursor = -1;      // row index within s_views[s_view_index].items, -1 = none/not applicable
std::vector<View> s_views;
bool s_has_fetched = false;  // distinguishes "fetched, turned out empty" from "never fetched"
bool s_wifi_connected = false;

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

// Fetches and parses GET /api/device/views. Only ever called from
// DeviceNavTask -- this is a blocking network call.
bool FetchViews(std::vector<View>* out_views) {
    const std::string url = BaseUrl() + "/api/device/views";
    ResponseBuffer response_buffer;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
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
        cJSON* field = cJSON_GetObjectItemCaseSensitive(view_json, "plugin_id");
        view.plugin_id = cJSON_IsString(field) ? field->valuestring : "";
        field = cJSON_GetObjectItemCaseSensitive(view_json, "view_type");
        view.view_type = cJSON_IsString(field) ? field->valuestring : "";
        field = cJSON_GetObjectItemCaseSensitive(view_json, "title");
        view.title = cJSON_IsString(field) ? field->valuestring : "";
        field = cJSON_GetObjectItemCaseSensitive(view_json, "body");
        view.body = cJSON_IsString(field) ? field->valuestring : "";

        cJSON* items_json = cJSON_GetObjectItemCaseSensitive(view_json, "items");
        cJSON* item_json = nullptr;
        cJSON_ArrayForEach(item_json, items_json) {
            Item item;
            field = cJSON_GetObjectItemCaseSensitive(item_json, "id");
            item.id = cJSON_IsString(field) ? field->valuestring : "";
            field = cJSON_GetObjectItemCaseSensitive(item_json, "text");
            item.text = cJSON_IsString(field) ? field->valuestring : "";
            field = cJSON_GetObjectItemCaseSensitive(item_json, "secondary");
            item.secondary = cJSON_IsString(field) ? field->valuestring : "";
            field = cJSON_GetObjectItemCaseSensitive(item_json, "checked");
            item.checkable = cJSON_IsBool(field);
            item.checked = item.checkable && cJSON_IsTrue(field);
            view.items.push_back(std::move(item));
        }
        views.push_back(std::move(view));
    }
    cJSON_Delete(root);

    *out_views = std::move(views);
    return true;
}

// POSTs the toggle. Only ever called from DeviceNavTask. Best-effort: on
// failure we just log it. The optimistic local toggle (OnConfirm, below)
// already happened, so the device and backend can drift until the next
// refresh -- an accepted tradeoff for how small the blast radius is here
// (a todo that looks done on-device until you next page through it).
void PostDeviceAction(const std::string& plugin_id, const std::string& item_id) {
    const std::string url = BaseUrl() + "/api/plugins/" + plugin_id + "/device-action";
    const std::string body = "{\"item_id\":\"" + item_id + "\"}";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kHttpTimeoutMs;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(kTag, "http client init failed for device-action");
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(kTag, "POST device-action failed: plugin=%s item=%s err=%s http=%d",
                 plugin_id.c_str(), item_id.c_str(), esp_err_to_name(err), status);
    }
}

// Builds the ui:: row list for the current view and shows it (or shows
// the home screen if s_view_index is -1). Caller must hold s_state_mutex.
void RenderCurrentLocked() {
    if (s_view_index < 0 || s_view_index >= static_cast<int>(s_views.size())) {
        const int total_pages = s_has_fetched ? static_cast<int>(s_views.size()) + 1 : 0;
        ui::ShowIdle(s_wifi_connected, total_pages);
        return;
    }
    const View& view = s_views[s_view_index];
    std::vector<ui::DeviceViewRow> rows;
    for (size_t i = 0; i < view.items.size(); ++i) {
        const Item& item = view.items[i];
        ui::DeviceViewRow row;
        row.text = item.text;
        row.secondary = item.secondary;
        row.checkable = item.checkable;
        row.checked = item.checked;
        row.highlighted = (static_cast<int>(i) == s_cursor);
        rows.push_back(row);
    }
    ui::ShowDeviceView(view.title, rows);
}

void EnqueueRefresh(bool enter_from_end) {
    if (s_refresh_in_flight) {
        return;  // already fetching; let that one land rather than piling up
    }
    s_refresh_in_flight = true;
    Command command = {};
    command.type = CommandType::kRefresh;
    command.enter_from_end = enter_from_end;
    xQueueSend(s_queue, &command, 0);
}

void EnqueuePostAction(const std::string& plugin_id, const std::string& item_id) {
    Command command = {};
    command.type = CommandType::kPostAction;
    CopyBounded(command.plugin_id, sizeof(command.plugin_id), plugin_id);
    CopyBounded(command.item_id, sizeof(command.item_id), item_id);
    xQueueSend(s_queue, &command, 0);
}

void HandleRefresh(bool enter_from_end) {
    ui::ShowLoading();
    std::vector<View> views;
    const bool ok = FetchViews(&views);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_refresh_in_flight = false;
    if (!ok) {
        s_view_index = -1;
        s_cursor = -1;
        RenderCurrentLocked();
        xSemaphoreGive(s_state_mutex);
        return;
    }
    s_has_fetched = true;
    if (views.empty()) {
        s_view_index = -1;
        s_cursor = -1;
        RenderCurrentLocked();
        xSemaphoreGive(s_state_mutex);
        return;
    }
    s_views = std::move(views);
    s_view_index = enter_from_end ? static_cast<int>(s_views.size()) - 1 : 0;
    s_cursor = s_views[s_view_index].items.empty()
                   ? -1
                   : (enter_from_end ? static_cast<int>(s_views[s_view_index].items.size()) - 1
                                     : 0);
    RenderCurrentLocked();
    xSemaphoreGive(s_state_mutex);
}

// Applies a toggle to the highlighted row. Toggling means "done" for every
// checkable view so far (todo), so the item leaves the list, matching
// device_view() only returning open items. Caller must hold s_state_mutex.
bool ConfirmCursorLocked(std::string* plugin_id, std::string* item_id) {
    if (s_view_index < 0 || s_cursor < 0) {
        return false;
    }
    View& view = s_views[s_view_index];
    if (s_cursor >= static_cast<int>(view.items.size()) || !view.items[s_cursor].checkable) {
        return false;
    }
    *plugin_id = view.plugin_id;
    *item_id = view.items[s_cursor].id;
    view.items.erase(view.items.begin() + s_cursor);
    if (view.items.empty()) {
        s_cursor = -1;
    } else if (s_cursor >= static_cast<int>(view.items.size())) {
        s_cursor = static_cast<int>(view.items.size()) - 1;
    }
    return true;
}

void DeviceNavTask(void*) {
    Command command;
    while (true) {
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (command.type) {
            case CommandType::kRefresh:
                HandleRefresh(command.enter_from_end);
                break;
            case CommandType::kPostAction:
                PostDeviceAction(command.plugin_id, command.item_id);
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
        xTaskCreate(DeviceNavTask, "device_nav", kTaskStackWords, nullptr, 4, &s_task);
    if (created != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "initialized");
    return ESP_OK;
}

void SetWifiConnected(bool connected) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_wifi_connected = connected;
    xSemaphoreGive(s_state_mutex);
}

int KnownPluginViewCount() {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const int count = s_has_fetched ? static_cast<int>(s_views.size()) : -1;
    xSemaphoreGive(s_state_mutex);
    return count;
}

void ResetToHome() {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_view_index = -1;
    s_cursor = -1;
    xSemaphoreGive(s_state_mutex);
}

void OnDown() {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_view_index < 0) {
        xSemaphoreGive(s_state_mutex);
        EnqueueRefresh(/*enter_from_end=*/false);
        return;
    }

    const View& view = s_views[s_view_index];
    if (s_cursor >= 0 && s_cursor + 1 < static_cast<int>(view.items.size())) {
        ++s_cursor;
        RenderCurrentLocked();
        xSemaphoreGive(s_state_mutex);
        return;
    }

    // Past the last row (or no rows at all): advance to the next view, or
    // home if this was the last one.
    ++s_view_index;
    if (s_view_index >= static_cast<int>(s_views.size())) {
        s_view_index = -1;
        s_cursor = -1;
    } else {
        s_cursor = s_views[s_view_index].items.empty() ? -1 : 0;
    }
    RenderCurrentLocked();
    xSemaphoreGive(s_state_mutex);
}

void OnUp() {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_view_index < 0) {
        xSemaphoreGive(s_state_mutex);
        EnqueueRefresh(/*enter_from_end=*/true);
        return;
    }

    if (s_cursor > 0) {
        --s_cursor;
        RenderCurrentLocked();
        xSemaphoreGive(s_state_mutex);
        return;
    }

    // Before the first row (or no rows at all): go to the previous view's
    // last row, or home if this was the first one.
    --s_view_index;
    if (s_view_index < 0) {
        s_cursor = -1;
    } else {
        s_cursor = s_views[s_view_index].items.empty()
                       ? -1
                       : static_cast<int>(s_views[s_view_index].items.size()) - 1;
    }
    RenderCurrentLocked();
    xSemaphoreGive(s_state_mutex);
}

bool OnConfirm() {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    std::string plugin_id;
    std::string item_id;
    const bool confirmed = ConfirmCursorLocked(&plugin_id, &item_id);
    if (confirmed) {
        RenderCurrentLocked();
    }
    xSemaphoreGive(s_state_mutex);

    if (confirmed) {
        EnqueuePostAction(plugin_id, item_id);
    }
    return confirmed;
}

void OnTap(int x, int y) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const int row = s_view_index < 0 ? -1 : ui::HitTestDeviceViewRow(x, y);
    if (row < 0 || row >= static_cast<int>(s_views[s_view_index].items.size())) {
        xSemaphoreGive(s_state_mutex);
        return;
    }

    s_cursor = row;
    std::string plugin_id;
    std::string item_id;
    const bool confirmed = ConfirmCursorLocked(&plugin_id, &item_id);
    RenderCurrentLocked();
    xSemaphoreGive(s_state_mutex);

    if (confirmed) {
        EnqueuePostAction(plugin_id, item_id);
    }
}

}  // namespace device_nav
