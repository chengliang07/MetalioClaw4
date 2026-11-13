#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lwip/sockets.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "application.h"
#include "i18n.h"
#include "system_info.h"

#define TAG "main"

// 调试通道：把关键日志同时通过 UDP 广播到局域网（8266），
// 避免 USB Serial/JTAG 句柄在运行中失效导致无法抓日志。
static int s_diag_udp_sock = -1;
static vprintf_like_t s_prev_log_vprintf = nullptr;
static bool s_diag_udp_enabled = false;

// 必须在网络（lwIP）初始化完成后再打开，否则 socket() 会导致启动崩溃。
void DiagLogEnableUdp(bool enable) {
    s_diag_udp_enabled = enable;
}

static void DiagUdpSend(const char* line) {
    if (!s_diag_udp_enabled) {
        return;
    }
    if (s_diag_udp_sock < 0) {
        s_diag_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (s_diag_udp_sock < 0) {
            return;
        }
        int enable = 1;
        setsockopt(s_diag_udp_sock, SOL_SOCKET, SO_BROADCAST, &enable,
                   sizeof(enable));
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8266);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s_diag_udp_sock, line, strlen(line), 0,
           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

static int DiagLogVprintf(const char* fmt, va_list ap) {
    char buf[256];
    va_list copy;
    va_copy(copy, ap);
    const int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > 0 &&
        (strstr(buf, "echo diag") != nullptr ||
         strstr(buf, "aec ref delay") != nullptr ||
         strstr(buf, "STATE:") != nullptr)) {
        DiagUdpSend(buf);
    }
    if (s_prev_log_vprintf != nullptr) {
        return s_prev_log_vprintf(fmt, ap);
    }
    return vprintf(fmt, ap);
}

extern "C" void app_main(void)
{
    s_prev_log_vprintf = esp_log_set_vprintf(DiagLogVprintf);

    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // UI locale from NVS (must run before first screen create).
    I18n::Init();

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
