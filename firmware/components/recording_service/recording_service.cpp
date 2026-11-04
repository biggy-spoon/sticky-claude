#include "recording_service.h"

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pdm_mic.h"

namespace recording_service {
namespace {

constexpr const char* kTag = "recording_service";
constexpr size_t kMaxSamples =
    (static_cast<uint64_t>(pdm_mic::kSampleRateHz) * kMaxRecordingMs) / 1000U;
constexpr size_t kChunkSamples = 320;  // 20ms at 16kHz
constexpr uint32_t kReadTimeoutMs = 100;
constexpr uint32_t kCaptureTaskStackWords = 3072;

struct WavHeader {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};
static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");

int16_t* s_buffer = nullptr;      // PSRAM, kMaxSamples entries
size_t s_sample_count = 0;        // samples captured in the in-progress/last clip
volatile bool s_capturing = false;
TaskHandle_t s_capture_task = nullptr;

void CaptureTask(void*) {
    int16_t chunk[kChunkSamples];

    while (true) {
        if (!s_capturing) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t samples_read = 0;
        const esp_err_t err = pdm_mic::Read(chunk, kChunkSamples, &samples_read, kReadTimeoutMs);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(kTag, "mic read failed: %s", esp_err_to_name(err));
            continue;
        }
        if (samples_read == 0) {
            continue;
        }

        const size_t space_left = kMaxSamples - s_sample_count;
        const size_t to_copy = samples_read < space_left ? samples_read : space_left;
        if (to_copy > 0) {
            memcpy(s_buffer + s_sample_count, chunk, to_copy * sizeof(int16_t));
            s_sample_count += to_copy;
        }
        if (s_sample_count >= kMaxSamples) {
            ESP_LOGW(kTag, "hit max recording length (%lums), stopping capture",
                     static_cast<unsigned long>(kMaxRecordingMs));
            s_capturing = false;
            pdm_mic::Disable();
        }
    }
}

WavHeader BuildWavHeader(size_t sample_count) {
    const uint32_t data_size = static_cast<uint32_t>(sample_count * sizeof(int16_t));
    WavHeader header = {};
    memcpy(header.riff, "RIFF", 4);
    header.chunk_size = 36 + data_size;
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmt, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1;  // PCM
    header.num_channels = 1;
    header.sample_rate = pdm_mic::kSampleRateHz;
    header.byte_rate = pdm_mic::kSampleRateHz * sizeof(int16_t);
    header.block_align = sizeof(int16_t);
    header.bits_per_sample = 16;
    memcpy(header.data, "data", 4);
    header.data_size = data_size;
    return header;
}

}  // namespace

esp_err_t Init() {
    esp_err_t err = pdm_mic::Init();
    if (err != ESP_OK) {
        return err;
    }

    if (s_buffer == nullptr) {
        s_buffer = static_cast<int16_t*>(
            heap_caps_malloc(kMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
        if (s_buffer == nullptr) {
            ESP_LOGE(kTag, "failed to allocate %u sample PSRAM buffer",
                     static_cast<unsigned>(kMaxSamples));
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_capture_task == nullptr) {
        const BaseType_t created = xTaskCreate(CaptureTask, "rec_capture",
                                               kCaptureTaskStackWords, nullptr, 6, &s_capture_task);
        if (created != pdPASS) {
            s_capture_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(kTag, "initialized: max_samples=%u (%lums)", static_cast<unsigned>(kMaxSamples),
             static_cast<unsigned long>(kMaxRecordingMs));
    return ESP_OK;
}

esp_err_t StartCapture() {
    if (s_buffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = pdm_mic::Enable();
    if (err != ESP_OK) {
        return err;
    }

    s_sample_count = 0;
    s_capturing = true;
    return ESP_OK;
}

esp_err_t StopCapture(size_t* out_sample_count) {
    s_capturing = false;
    pdm_mic::Disable();
    if (out_sample_count != nullptr) {
        *out_sample_count = s_sample_count;
    }
    return ESP_OK;
}

esp_err_t BuildWav(uint8_t** out_wav, size_t* out_wav_len) {
    if (out_wav == nullptr || out_wav_len == nullptr || s_sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t data_bytes = s_sample_count * sizeof(int16_t);
    const size_t total_bytes = sizeof(WavHeader) + data_bytes;
    uint8_t* wav = static_cast<uint8_t*>(malloc(total_bytes));
    if (wav == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const WavHeader header = BuildWavHeader(s_sample_count);
    memcpy(wav, &header, sizeof(header));
    memcpy(wav + sizeof(header), s_buffer, data_bytes);

    *out_wav = wav;
    *out_wav_len = total_bytes;
    return ESP_OK;
}

}  // namespace recording_service
