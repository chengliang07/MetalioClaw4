#include "afe_wake_word.h"
#include "audio_service.h"

#include <assert.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <opus_encoder.h>
#include <cmath>
#include "application.h"

extern Application* g_barge_in_app;
extern std::atomic<bool> g_barge_in_pending;
#include <memory>
#include <sstream>
#include <cstring>

#define DETECTION_RUNNING_EVENT (1 << 0)
#define DETECTION_TASK_EXIT     (1 << 1)

#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {
    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    Deinitialize();

    if (detect_task_ != nullptr) {
        xEventGroupSetBits(event_group_, DETECTION_TASK_EXIT);
        // detection task 自行 vTaskDelete；稍等其退出后再拆 event group
        for (int i = 0; i < 50 && detect_task_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        detect_task_ = nullptr;
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
        wake_word_encode_task_stack_ = nullptr;
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
        wake_word_encode_task_buffer_ = nullptr;
    }

    if (models_owned_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
        models_ = nullptr;
    }

    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    if (afe_data_ != nullptr) {
        ESP_LOGW(TAG, "Initialize skipped: AFE already exists");
        return true;
    }

    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    if (models_ == nullptr) {
        if (models_list == nullptr) {
            models_ = esp_srmodel_init("model");
            models_owned_ = true;
        } else {
            models_ = models_list;
            models_owned_ = false;
        }
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

    if (wake_words_.empty()) {
        for (int i = 0; i < models_->num; i++) {
            ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
            if (strstr(models_->model_name[i], ESP_WN_PREFIX) != nullptr) {
                wakenet_model_ = models_->model_name[i];
                auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
                std::stringstream ss(words);
                std::string word;
                while (std::getline(ss, word, ';')) {
                    wake_words_.push_back(word);
                }
            }
        }
    }

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    afe_config_t* afe_config =
        afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "create_from_config failed");
        afe_iface_ = nullptr;
        return false;
    }

    // detection task 只建一次；Deinitialize 只毁 AFE，不毁任务。
    if (detect_task_ == nullptr) {
        xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<AfeWakeWord*>(arg);
                self->AudioDetectionTask();
                self->detect_task_ = nullptr;
                vTaskDelete(nullptr);
            },
            "audio_detection", 4096, this, 3, &detect_task_);
    }

    ESP_LOGI(TAG, "Wake word AFE created");
    return true;
}

void AfeWakeWord::Deinitialize() {
    Stop();

    // 等 detection 离开 fetch（fetch_with_delay 最长约 50ms）
    for (int i = 0; i < 40 && fetching_.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (fetching_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Deinitialize: detection still fetching, proceeding anyway");
    }

    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Destroying wake word AFE (stops internal AFE tasks)");
    afe_iface_->destroy(afe_data_);
    afe_data_ = nullptr;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::SetBargeInVadCallback(std::function<void()> callback) {
    barge_in_vad_callback_ = std::move(callback);
    barge_in_vad_enabled_.store(callback != nullptr, std::memory_order_relaxed);
}

std::vector<std::vector<int16_t>> AfeWakeWord::TakeBargeInPreroll() {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    std::vector<std::vector<int16_t>> preroll;
    preroll.reserve(wake_word_pcm_.size());
    for (auto& frame : wake_word_pcm_) {
        preroll.emplace_back(std::move(frame));
    }
    wake_word_pcm_.clear();
    return preroll;
}

void AfeWakeWord::Start() {
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        ESP_LOGW(TAG, "Start ignored: AFE not initialized");
        return;
    }
    afe_iface_->enable_wakenet(afe_data_);
    speech_frames_ = 0;
    is_speaking_ = false;
    barge_in_warmup_frames_ = 0;
    barge_in_rms_baseline_ = 500;
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    speech_frames_ = 0;
    is_speaking_ = false;
    barge_in_warmup_frames_ = 0;
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ != nullptr && afe_iface_ != nullptr) {
        afe_iface_->disable_wakenet(afe_data_);
    }
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize() {
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    ESP_LOGI(TAG, "Audio detection task started");

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, DETECTION_RUNNING_EVENT | DETECTION_TASK_EXIT, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (bits & DETECTION_TASK_EXIT) {
            break;
        }

        const esp_afe_sr_iface_t* iface = nullptr;
        esp_afe_sr_data_t* data = nullptr;
        {
            std::lock_guard<std::mutex> lock(afe_mutex_);
            if (afe_data_ == nullptr || afe_iface_ == nullptr) {
                xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
                continue;
            }
            // 在持锁时置位，避免与 Deinitialize 的「等 fetching_」窗口竞态
            fetching_.store(true, std::memory_order_release);
            iface = afe_iface_;
            data = afe_data_;
        }

        auto res = iface->fetch_with_delay(data, pdMS_TO_TICKS(50));
        fetching_.store(false, std::memory_order_release);

        if ((xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) == 0) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(afe_mutex_);
            if (afe_data_ != data) {
                continue;
            }
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;
        }

        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (barge_in_vad_enabled_.load(std::memory_order_relaxed) &&
            res->data != nullptr && res->data_size > 0) {
            const auto* samples = reinterpret_cast<const int16_t*>(res->data);
            const size_t count = res->data_size / sizeof(int16_t);
            uint64_t sum = 0;
            for (size_t i = 0; i < count; ++i) {
                sum += static_cast<uint64_t>(static_cast<int32_t>(samples[i]) * samples[i]);
            }
            const uint32_t frame_rms = static_cast<uint32_t>(
                sqrt(static_cast<double>(sum) / count));
            // SR AFE 不保证输出可靠 vad_state；用 AEC 后能量相对基线的
            // 持续突增作为插话证据，避免依赖未启用的 VAD 模型。
            const bool speech = barge_in_warmup_frames_ >= 10 &&
                                frame_rms > barge_in_rms_baseline_ * 2 &&
                                frame_rms > 300;
            if (barge_in_warmup_frames_ < 10) {
                ++barge_in_warmup_frames_;
            }
            if (speech) {
                if (speech_frames_ < 3) {
                    ++speech_frames_;
                }
                if (!is_speaking_ && speech_frames_ >= 2) {
                    is_speaking_ = true;
                    if (g_barge_in_app != nullptr) {
                        g_barge_in_pending.store(true, std::memory_order_relaxed);
                        xEventGroupSetBits(g_barge_in_app->GetEventGroup(),
                                           MAIN_EVENT_BARGE_IN);
                    }
                }
            } else {
                speech_frames_ = 0;
                is_speaking_ = false;
            }
            if (frame_rms < barge_in_rms_baseline_) {
                const uint32_t next = (barge_in_rms_baseline_ * 99 + frame_rms) / 100;
                barge_in_rms_baseline_ = next < 100 ? 100 : next;
            }
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            Stop();
            last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];
            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }

    ESP_LOGI(TAG, "Audio detection task exit");
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ =
            (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ =
            (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic(
        [](void* arg) {
            auto* this_ = (AfeWakeWord*)arg;
            {
                auto start_time = esp_timer_get_time();
                auto encoder =
                    std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
                encoder->SetComplexity(0);

                int packets = 0;
                std::vector<std::vector<int16_t>> pcm_frames;
                {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    pcm_frames.reserve(this_->wake_word_pcm_.size());
                    for (auto& pcm : this_->wake_word_pcm_) {
                        pcm_frames.emplace_back(std::move(pcm));
                    }
                    this_->wake_word_pcm_.clear();
                }
                for (auto& pcm : pcm_frames) {
                    encoder->Encode(std::move(pcm), [this_](std::vector<uint8_t>&& opus) {
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(std::move(opus));
                        this_->wake_word_cv_.notify_all();
                    });
                    packets++;
                }

                auto end_time = esp_timer_get_time();
                ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets,
                         (long)((end_time - start_time) / 1000));

                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
            }
            vTaskDelete(NULL);
        },
        "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_,
        wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
