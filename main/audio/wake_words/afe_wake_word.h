#ifndef AFE_WAKE_WORD_H
#define AFE_WAKE_WORD_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include <model_path.h>

#include <atomic>
#include <deque>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "audio_codec.h"
#include "wake_word.h"

class AfeWakeWord : public WakeWord {
public:
    AfeWakeWord();
    ~AfeWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Deinitialize() override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }
    void SetBargeInVadCallback(std::function<void()> callback) override;
    std::vector<std::vector<int16_t>> TakeBargeInPreroll() override;

private:
    srmodel_list_t* models_ = nullptr;
    bool models_owned_ = false;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    char* wakenet_model_ = nullptr;
    std::vector<std::string> wake_words_;
    EventGroupHandle_t event_group_;
    TaskHandle_t detect_task_ = nullptr;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    std::function<void()> barge_in_vad_callback_;
    std::atomic<bool> barge_in_vad_enabled_{false};
    int speech_frames_ = 0;
    bool is_speaking_ = false;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    std::deque<std::vector<int16_t>> wake_word_pcm_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;
    // 保护 afe_data_ 的 Feed / GetFeedSize / destroy；fetch 用 fetching_ 与 destroy 握手。
    std::mutex afe_mutex_;
    std::atomic<bool> fetching_{false};
    uint32_t barge_in_rms_baseline_ = 100;

    void StoreWakeWordData(const int16_t* data, size_t size);
    void AudioDetectionTask();
};

#endif
