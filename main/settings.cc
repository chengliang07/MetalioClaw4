#include "settings.h"

#include <functional>

#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#define TAG "Settings"

// NVS 写入会关闭 Cache 并访问 Flash，调用任务的栈必须在内部 RAM。
// 部分组件任务（SDIO/4G 等）的栈在 PSRAM，直接写 NVS 会触发
// spi_flash_disable_interrupts_caches_and_other_cpu 断言并重启。
static void RunNvsWrite(const std::function<void()>& fn) {
    if (esp_ptr_internal(esp_cpu_get_sp())) {
        fn();
        return;
    }

    struct Job {
        const std::function<void()>* fn;
        SemaphoreHandle_t done;
    };
    Job job{&fn, xSemaphoreCreateBinary()};
    if (job.done == nullptr) {
        fn();
        return;
    }
    BaseType_t created = xTaskCreate(
        [](void* arg) {
            auto* j = static_cast<Job*>(arg);
            (*j->fn)();
            xSemaphoreGive(j->done);
            vTaskDelete(nullptr);
        },
        "nvs_write", 4096, &job, 5, nullptr);
    if (created != pdPASS) {
        vSemaphoreDelete(job.done);
        fn();
        return;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
}

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            nvs_handle_t handle = nvs_handle_;
            RunNvsWrite([handle]() { ESP_ERROR_CHECK(nvs_commit(handle)); });
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    value.resize(length);
    ESP_ERROR_CHECK(nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length));
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (read_write_) {
        nvs_handle_t handle = nvs_handle_;
        RunNvsWrite([handle, key, value]() {
            ESP_ERROR_CHECK(nvs_set_str(handle, key.c_str(), value.c_str()));
        });
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (read_write_) {
        nvs_handle_t handle = nvs_handle_;
        RunNvsWrite([handle, key, value]() {
            ESP_ERROR_CHECK(nvs_set_i32(handle, key.c_str(), value));
        });
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (read_write_) {
        nvs_handle_t handle = nvs_handle_;
        RunNvsWrite([handle, key, value]() {
            ESP_ERROR_CHECK(nvs_set_u8(handle, key.c_str(), value ? 1 : 0));
        });
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseKey(const std::string& key) {
    if (read_write_) {
        nvs_handle_t handle = nvs_handle_;
        RunNvsWrite([handle, key]() {
            auto ret = nvs_erase_key(handle, key.c_str());
            if (ret != ESP_ERR_NVS_NOT_FOUND) {
                ESP_ERROR_CHECK(ret);
            }
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseAll() {
    if (read_write_) {
        nvs_handle_t handle = nvs_handle_;
        RunNvsWrite([handle]() { ESP_ERROR_CHECK(nvs_erase_all(handle)); });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}
