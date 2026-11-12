#include "settings.h"

#include <functional>
#include <mutex>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <nvs_flash.h>

#define TAG "Settings"

// NVS 写入会关闭 Cache 并访问 Flash；LVGL/其他组件的栈在 PSRAM 时
// 直接写会触发 spi_flash_disable_interrupts_caches_and_other_cpu 断言。
// 统一投递到内部 RAM 栈专用任务，且串行执行避免并发写 Flash。
struct NvsWriteJob {
    std::string ns;
    std::function<void(nvs_handle_t)> fn;
    SemaphoreHandle_t done;
};

QueueHandle_t s_write_queue = nullptr;
StaticTask_t s_writer_tcb;
StackType_t* s_writer_stack = nullptr;

void NvsWriterTask(void* /*arg*/) {
    NvsWriteJob* job = nullptr;
    for (;;) {
        if (xQueueReceive(s_write_queue, &job, portMAX_DELAY) != pdTRUE ||
            job == nullptr) {
            continue;
        }
        nvs_handle_t handle = 0;
        if (nvs_open(job->ns.c_str(), NVS_READWRITE, &handle) == ESP_OK) {
            job->fn(handle);
            nvs_commit(handle);
            nvs_close(handle);
        }
        if (job->done != nullptr) {
            xSemaphoreGive(job->done);
        } else {
            delete job;
        }
        job = nullptr;
    }
}

QueueHandle_t GetWriteQueue() {
    static std::once_flag once;
    std::call_once(once, []() {
        s_write_queue = xQueueCreate(12, sizeof(NvsWriteJob*));
        if (s_write_queue != nullptr && s_writer_stack == nullptr) {
            s_writer_stack = static_cast<StackType_t*>(heap_caps_malloc(
                4096 * sizeof(StackType_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (s_write_queue != nullptr && s_writer_stack != nullptr) {
            xTaskCreateStatic(NvsWriterTask, "nvs_writer", 4096, nullptr, 5,
                              s_writer_stack, &s_writer_tcb);
        }
    });
    return s_write_queue;
}

void Settings::EnqueueWrite(
    const std::string& ns,
    const std::function<void(nvs_handle_t)>& fn) {
    QueueHandle_t queue = GetWriteQueue();
    if (queue == nullptr) {
        return;
    }
    auto* job = new NvsWriteJob{ns, fn, xSemaphoreCreateBinary()};
    if (job->done == nullptr) {
        delete job;
        return;
    }
    if (xQueueSend(queue, &job, portMAX_DELAY) != pdTRUE) {
        if (job->done != nullptr) {
            vSemaphoreDelete(job->done);
        }
        delete job;
        return;
    }
    xSemaphoreTake(job->done, portMAX_DELAY);
    vSemaphoreDelete(job->done);
    delete job;
}

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
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
        EnqueueWrite(ns_, [key, value](nvs_handle_t handle) {
            nvs_set_str(handle, key.c_str(), value.c_str());
        });
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
        EnqueueWrite(ns_, [key, value](nvs_handle_t handle) {
            nvs_set_i32(handle, key.c_str(), value);
        });
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
        EnqueueWrite(ns_, [key, value](nvs_handle_t handle) {
            nvs_set_u8(handle, key.c_str(), value ? 1 : 0);
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseKey(const std::string& key) {
    if (read_write_) {
        EnqueueWrite(ns_, [key](nvs_handle_t handle) {
            auto ret = nvs_erase_key(handle, key.c_str());
            (void)ret;
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseAll() {
    if (read_write_) {
        EnqueueWrite(ns_, [](nvs_handle_t handle) {
            nvs_erase_all(handle);
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}
