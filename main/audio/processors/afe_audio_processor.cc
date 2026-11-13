#include "afe_audio_processor.h"
#include <cmath>
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    // 打断开启时 AFE 与 TTS 同时运行。VAD 用最不敏感的档位，
    // 避免扬声器残留回声被当成用户说话而反复自打断。
    afe_config->vad_mode = VAD_MODE_3;
    afe_config->vad_min_noise_ms = 300;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    // AEC + VAD 均在创建时初始化；运行时动态启停会重分配 AFE 内部缓冲，
    // 在 P4 + PSRAM 上容易触发重启。
    afe_config->aec_init = true;
    afe_config->vad_init = true;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, 3, NULL);
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    // 回声诊断：累计输入麦克风与参考通道能量（1 秒打印一次）
    if (codec_ != nullptr && codec_->input_channels() >= 2) {
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            const int32_t mic = data[i];
            const int32_t ref = data[i + 1];
            diag_mic_sq_ += static_cast<uint64_t>(mic * mic);
            diag_ref_sq_ += static_cast<uint64_t>(ref * ref);
            diag_in_samples_++;
        }
        diag_frames_++;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    // 重新进入采集时清掉上一轮的 VAD/音频残留，避免旧状态立即触发打断。
    is_speaking_ = false;
    speech_frames_ = 0;
    output_buffer_.clear();
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        if (res->data != nullptr && res->data_size > 0) {
            const int16_t* out = reinterpret_cast<const int16_t*>(res->data);
            const int out_samples = res->data_size / static_cast<int>(sizeof(int16_t));
            for (int i = 0; i < out_samples; ++i) {
                const int32_t v = out[i];
                diag_out_sq_ += static_cast<uint64_t>(v * v);
                diag_out_samples_++;
            }
        }
        if (diag_in_samples_ >= 16000) {
            const uint32_t mic_rms = static_cast<uint32_t>(
                sqrt(static_cast<double>(diag_mic_sq_) / diag_in_samples_));
            const uint32_t ref_rms = static_cast<uint32_t>(
                sqrt(static_cast<double>(diag_ref_sq_) / diag_in_samples_));
            const uint32_t out_rms = diag_out_samples_ > 0
                ? static_cast<uint32_t>(
                      sqrt(static_cast<double>(diag_out_sq_) / diag_out_samples_))
                : 0;
            ESP_LOGI(TAG, "echo diag: mic=%u ref=%u aec_out=%u vad=%d",
                     mic_rms, ref_rms, out_rms,
                     res->vad_state == VAD_SPEECH ? 1 : 0);
            diag_frames_ = 0;
            diag_mic_sq_ = 0;
            diag_ref_sq_ = 0;
            diag_in_samples_ = 0;
            diag_out_sq_ = 0;
            diag_out_samples_ = 0;
        }

        // VAD state change：连续语音约 300ms 才上报，滤掉短促回声。
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH) {
                if (speech_frames_ < 20) {
                    speech_frames_++;
                }
                if (!is_speaking_ && speech_frames_ >= 10) {
                    is_speaking_ = true;
                    vad_state_change_callback_(true);
                }
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                speech_frames_ = 0;
                is_speaking_ = false;
                vad_state_change_callback_(false);
            } else if (res->vad_state == VAD_SILENCE) {
                speech_frames_ = 0;
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "EnableDeviceAec: AFE not initialized");
        return;
    }
    // AEC/VAD 已随实例常驻。这里只保留接口兼容，不再动态 enable/disable：
    // 动态切换会重分配 PSRAM 缓冲并导致重启。打断开关由上层决定是否
    // 使用 VAD 结果和上行门控。
    (void)enable;
}
