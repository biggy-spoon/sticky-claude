#include "captive_portal.h"

#include <cstring>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "settings_service.h"
#include "wifi_service.h"

namespace captive_portal {
namespace {

constexpr const char* kTag = "captive_portal";
constexpr const char* kApSsid = "StickyClaude-Setup";
constexpr uint16_t kDnsPort = 53;
constexpr uint32_t kDnsTaskStackWords = 3072;

// Minimal DNS server: answers every query with the AP's own IP, which is
// the standard trick that makes phones auto-pop the captive portal page.
void DnsServerTask(void*) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(kTag, "DNS socket create failed");
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(kDnsPort);
    if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        ESP_LOGE(kTag, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    uint32_t ap_ip = 0;
    inet_pton(AF_INET, "192.168.4.1", &ap_ip);

    uint8_t buffer[512];
    while (true) {
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        const int received = recvfrom(sock, buffer, sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (received < 12) {
            continue;
        }

        // Reuse the request buffer as the response: header fields first,
        // then the original question section is already in place.
        buffer[2] = 0x81;  // QR=1 (response), opcode=0, AA=0, TC=0, RD=1 (copied intent)
        buffer[3] = 0x80;  // RA=1, Z=0, RCODE=0 (no error)
        buffer[6] = 0x00;  // ANCOUNT = 1
        buffer[7] = 0x01;
        buffer[8] = 0x00;  // NSCOUNT = 0
        buffer[9] = 0x00;
        buffer[10] = 0x00;  // ARCOUNT = 0
        buffer[11] = 0x00;

        int answer_offset = received;
        // Answer: NAME = pointer to question at offset 12, TYPE=A, CLASS=IN,
        // TTL=60s, RDLENGTH=4, RDATA=AP IP.
        buffer[answer_offset++] = 0xC0;
        buffer[answer_offset++] = 0x0C;
        buffer[answer_offset++] = 0x00;
        buffer[answer_offset++] = 0x01;  // TYPE A
        buffer[answer_offset++] = 0x00;
        buffer[answer_offset++] = 0x01;  // CLASS IN
        buffer[answer_offset++] = 0x00;
        buffer[answer_offset++] = 0x00;
        buffer[answer_offset++] = 0x00;
        buffer[answer_offset++] = 0x3C;  // TTL 60
        buffer[answer_offset++] = 0x00;
        buffer[answer_offset++] = 0x04;  // RDLENGTH 4
        std::memcpy(buffer + answer_offset, &ap_ip, 4);
        answer_offset += 4;

        sendto(sock, buffer, answer_offset, 0, reinterpret_cast<sockaddr*>(&client_addr),
              client_len);
    }
}

std::string UrlDecode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            const std::string hex = encoded.substr(i + 1, 2);
            decoded += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

std::string ExtractField(const std::string& body, const std::string& key) {
    const std::string needle = key + "=";
    const size_t start = body.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    const size_t value_start = start + needle.size();
    size_t value_end = body.find('&', value_start);
    if (value_end == std::string::npos) {
        value_end = body.size();
    }
    return UrlDecode(body.substr(value_start, value_end - value_start));
}

const char* kSetupPageHtml = R"HTML(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>sticky-claude setup</title>
  <style>
    body { font-family: -apple-system, system-ui, sans-serif; background: #1a1a1a; color: #f0f0f0;
           margin: 0; padding: 24px 16px; }
    .card { max-width: 420px; margin: 0 auto; background: #242424; border-radius: 16px;
            padding: 24px; box-shadow: 0 4px 24px rgba(0,0,0,0.4); }
    h1 { font-size: 20px; margin: 0 0 4px; }
    p.sub { color: #999; margin: 0 0 24px; font-size: 14px; }
    label { display: block; font-size: 13px; color: #bbb; margin: 16px 0 6px; }
    input { width: 100%; box-sizing: border-box; padding: 12px; border-radius: 10px;
            border: 1px solid #3a3a3a; background: #1a1a1a; color: #f0f0f0; font-size: 15px; }
    input:focus { outline: none; border-color: #7b5cff; }
    button { width: 100%; margin-top: 24px; padding: 14px; border-radius: 10px; border: none;
             background: #7b5cff; color: white; font-size: 16px; font-weight: 600;
             cursor: pointer; }
    button:active { background: #6a4ce0; }
  </style>
</head>
<body>
  <div class="card">
    <h1>sticky-claude</h1>
    <p class="sub">Connect your device to WiFi and your backend</p>
    <form action="/save" method="POST">
      <label for="ssid">WiFi network name</label>
      <input id="ssid" name="ssid" required autocapitalize="off" autocorrect="off">
      <label for="password">WiFi password</label>
      <input id="password" name="password" type="password">
      <label for="backend_url">Backend URL</label>
      <input id="backend_url" name="backend_url" placeholder="http://192.168.1.50:8000/converse"
             required autocapitalize="off" autocorrect="off">
      <button type="submit">Save and connect</button>
    </form>
  </div>
</body>
</html>
)HTML";

const char* kSavedPageHtml = R"HTML(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>sticky-claude setup</title>
  <style>
    body { font-family: -apple-system, system-ui, sans-serif; background: #1a1a1a; color: #f0f0f0;
           display: flex; align-items: center; justify-content: center; height: 100vh; margin: 0; }
    .card { text-align: center; padding: 24px; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Saved!</h1>
    <p>The device is restarting and will connect to your WiFi.</p>
  </div>
</body>
</html>
)HTML";

esp_err_t HandleGetSetupPage(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, kSetupPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandlePostSave(httpd_req_t* request) {
    std::string body(request->content_len, '\0');
    int received = 0;
    while (received < request->content_len) {
        const int chunk = httpd_req_recv(request, body.data() + received,
                                         request->content_len - received);
        if (chunk <= 0) {
            httpd_resp_send_500(request);
            return ESP_FAIL;
        }
        received += chunk;
    }

    settings_service::Settings settings = {};
    settings.wifi_ssid = ExtractField(body, "ssid");
    settings.wifi_password = ExtractField(body, "password");
    settings.backend_url = ExtractField(body, "backend_url");

    if (settings.wifi_ssid.empty() || settings.backend_url.empty()) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Missing required fields", HTTPD_RESP_USE_STRLEN);
    }

    settings_service::Save(settings);
    ESP_LOGI(kTag, "settings saved, rebooting");

    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, kSavedPageHtml, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(500));  // let the response flush before rebooting
    esp_restart();
    return ESP_OK;
}

// Catch-all so the various captive-portal probe paths (Android's
// /generate_204, Apple's /hotspot-detect.html, etc.) land on the setup page
// instead of a 404, which is what makes phones auto-open it.
esp_err_t HandleNotFound(httpd_req_t* request, httpd_err_code_t) {
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/");
    httpd_resp_send(request, nullptr, 0);
    return ESP_OK;
}

}  // namespace

esp_err_t Run() {
    std::string ap_ip;
    esp_err_t err = wifi_service::StartAp(kApSsid, &ap_ip);
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreate(DnsServerTask, "captive_dns", kDnsTaskStackWords, nullptr, 5, nullptr);

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = nullptr;
    err = httpd_start(&server, &http_config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t get_root = {.uri = "/", .method = HTTP_GET, .handler = HandleGetSetupPage,
                            .user_ctx = nullptr};
    httpd_uri_t post_save = {.uri = "/save", .method = HTTP_POST, .handler = HandlePostSave,
                             .user_ctx = nullptr};
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_save);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, HandleNotFound);

    ESP_LOGI(kTag, "portal ready: connect to WiFi \"%s\", browse to http://%s/", kApSsid,
             ap_ip.c_str());
    return ESP_OK;
}

}  // namespace captive_portal
