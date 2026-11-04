#include "assistant_service.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace assistant_service {
namespace {

constexpr const char* kTag = "assistant_service";
constexpr int kTimeoutMs = 60000;  // local STT + Claude round trip; generous for a Pi

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

std::string JsonStringField(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return {};
    }
    return item->valuestring;
}

}  // namespace

ConverseResult Converse(const uint8_t* wav_bytes, size_t wav_len) {
    ConverseResult result = {};
    if (wav_bytes == nullptr || wav_len == 0) {
        result.error_message = "empty audio";
        return result;
    }

    ResponseBuffer response_buffer;

    esp_http_client_config_t config = {};
    config.url = CONFIG_STICKY_CLAUDE_BACKEND_URL;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kTimeoutMs;
    config.event_handler = &HttpEventHandler;
    config.user_data = &response_buffer;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        result.error_message = "http client init failed";
        return result;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, reinterpret_cast<const char*>(wav_bytes),
                                   static_cast<int>(wav_len));

    const esp_err_t err = esp_http_client_perform(client);
    result.http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        result.error_message = esp_err_to_name(err);
        ESP_LOGW(kTag, "request failed: %s", result.error_message.c_str());
        return result;
    }

    if (result.http_status < 200 || result.http_status >= 300) {
        result.error_message = response_buffer.body.empty() ? "backend error"
                                                             : response_buffer.body;
        ESP_LOGW(kTag, "backend returned http=%d: %s", result.http_status,
                 result.error_message.c_str());
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(response_buffer.body.c_str(), response_buffer.body.size());
    if (root == nullptr) {
        result.error_message = "invalid JSON from backend";
        return result;
    }

    result.transcript = JsonStringField(root, "transcript");
    result.reply = JsonStringField(root, "reply");
    cJSON_Delete(root);

    result.success = !result.reply.empty();
    if (!result.success) {
        result.error_message = "backend returned no reply";
    }
    return result;
}

}  // namespace assistant_service
