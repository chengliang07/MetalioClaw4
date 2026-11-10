#include "podcast_player.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "audio_codec.h"
#include "board.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_crt_bundle.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_rate_cvt.h"
#endif

namespace {

constexpr const char* TAG = "PodcastPlayer";
constexpr int kOutputSampleRate = 16000;

struct PlayArg {
    uint32_t generation = 0;
    std::string url;
};

std::mutex s_player_mutex;
std::mutex s_api_mutex;
esp_asp_handle_t s_player = nullptr;
TaskHandle_t s_play_task = nullptr;
std::atomic<uint32_t> s_generation{0};
std::atomic<int> s_pcm_channels{2};
std::atomic<bool> s_output_enabled{false};
std::atomic<bool> s_reported_playing{false};
PodcastPlayerEventCallback s_event_cb = nullptr;
void* s_event_ctx = nullptr;
std::vector<int16_t> s_pcm_buf;
bool s_media_lib_ready = false;

void Emit(PodcastPlayerEvent event, const char* detail) {
    PodcastPlayerEventCallback callback = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        callback = s_event_cb;
        ctx = s_event_ctx;
    }
    if (callback != nullptr) {
        callback(event, detail, ctx);
    }
}

void EnsureMediaLibAdapter() {
    if (!s_media_lib_ready) {
        media_lib_add_default_adapter();
        s_media_lib_ready = true;
    }
}

void FinishPlayTask() {
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        if (s_play_task == xTaskGetCurrentTaskHandle()) {
            s_play_task = nullptr;
        }
    }
    vTaskDelete(nullptr);
}

void ApplyRateCvt16k(esp_asp_handle_t player) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    esp_gmf_element_handle_t rate_el = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipeline) !=
            ESP_GMF_ERR_OK ||
        pipeline == nullptr) {
        return;
    }
    if (esp_gmf_pipeline_get_el_by_name(pipeline, "aud_rate_cvt", &rate_el) ==
            ESP_GMF_ERR_OK &&
        rate_el != nullptr) {
        esp_gmf_rate_cvt_set_dest_rate(
            rate_el, static_cast<uint32_t>(kOutputSampleRate));
    }
#else
    (void)player;
#endif
}

bool RegisterHttpWithCertBundle(esp_asp_handle_t player) {
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http_cfg, &http_io) != ESP_GMF_ERR_OK ||
        http_io == nullptr) {
        return false;
    }
    if (esp_audio_simple_player_register_io(player, http_io) !=
        ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(http_io);
        return false;
    }
    return true;
}

int PodcastOutCallback(uint8_t* data, int data_size, void* ctx) {
    if (!s_output_enabled.load(std::memory_order_relaxed) || data == nullptr ||
        data_size <= 0) {
        return 0;
    }
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr) {
        return 0;
    }

    const int frames = data_size / static_cast<int>(sizeof(int16_t));
    if (frames <= 0) {
        return 0;
    }
    const auto* pcm = reinterpret_cast<const int16_t*>(data);
    const int channels = s_pcm_channels.load(std::memory_order_relaxed);

    if (channels >= 2) {
        const int mono = frames / channels;
        if (mono <= 0) {
            return 0;
        }
        s_pcm_buf.resize(static_cast<size_t>(mono));
        for (int i = 0; i < mono; ++i) {
            int sum = 0;
            for (int ch = 0; ch < channels; ++ch) {
                sum += pcm[i * channels + ch];
            }
            s_pcm_buf[static_cast<size_t>(i)] =
                static_cast<int16_t>(sum / channels);
        }
    } else {
        s_pcm_buf.assign(pcm, pcm + frames);
    }

    codec->OutputData(s_pcm_buf);
    if (!s_reported_playing.exchange(true, std::memory_order_acq_rel)) {
        Emit(PodcastPlayerEvent::Playing, nullptr);
    }
    return 0;
}

int PodcastEventCallback(esp_asp_event_pkt_t* event, void* /*ctx*/) {
    if (event == nullptr) {
        return 0;
    }
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
        event->payload_size >=
            static_cast<int>(sizeof(esp_asp_music_info_t))) {
        esp_asp_music_info_t info = {};
        std::memcpy(&info, event->payload, sizeof(info));
        if (info.channels > 0) {
            s_pcm_channels.store(info.channels, std::memory_order_relaxed);
        }
        ESP_LOGI(TAG, "music info: rate=%d ch=%d bits=%d", info.sample_rate,
                 info.channels, info.bits);
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE &&
               event->payload_size == sizeof(esp_asp_state_t)) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        std::memcpy(&state, event->payload, event->payload_size);
        if (state == ESP_ASP_STATE_RUNNING) {
            if (!s_reported_playing.exchange(true,
                                             std::memory_order_acq_rel)) {
                Emit(PodcastPlayerEvent::Playing, nullptr);
            }
        } else if (state == ESP_ASP_STATE_PAUSED) {
            Emit(PodcastPlayerEvent::Paused, nullptr);
        } else if (state == ESP_ASP_STATE_ERROR) {
            Emit(PodcastPlayerEvent::Error, "播放失败");
        }
    }
    return 0;
}

void PodcastPlayTask(void* arg) {
    auto* play_arg = static_cast<PlayArg*>(arg);
    const uint32_t generation = play_arg->generation;
    const std::string url = play_arg->url;
    delete play_arg;

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        Emit(PodcastPlayerEvent::Error, "音频输出不可用");
        FinishPlayTask();
        return;
    }

    EnsureMediaLibAdapter();
    s_pcm_channels.store(2, std::memory_order_relaxed);
    s_reported_playing.store(false, std::memory_order_relaxed);

    {
        esp_asp_cfg_t cfg = {
            .in = {},
            .out = {.cb = PodcastOutCallback, .user_ctx = codec},
            .task_prio = 5,
            .task_stack = 8 * 1024,
            .prev = nullptr,
            .prev_ctx = nullptr,
        };
        esp_asp_handle_t player = nullptr;
        if (esp_audio_simple_player_new(&cfg, &player) != ESP_GMF_ERR_OK ||
            player == nullptr) {
            Emit(PodcastPlayerEvent::Error, "创建播放器失败");
            FinishPlayTask();
            return;
        }
        ApplyRateCvt16k(player);
        if (!RegisterHttpWithCertBundle(player)) {
            esp_audio_simple_player_destroy(player);
            Emit(PodcastPlayerEvent::Error, "初始化 HTTPS 播放失败");
            FinishPlayTask();
            return;
        }
        esp_audio_simple_player_set_event(player, PodcastEventCallback,
                                          nullptr);

        bool cancelled = false;
        {
            std::lock_guard<std::mutex> api_lock(s_api_mutex);
            {
                std::lock_guard<std::mutex> lock(s_player_mutex);
                if (generation !=
                    s_generation.load(std::memory_order_relaxed)) {
                    cancelled = true;
                } else {
                    s_player = player;
                }
            }
            if (cancelled) {
                esp_audio_simple_player_destroy(player);
            }
        }
        if (cancelled) {
            FinishPlayTask();
            return;
        }

        codec->EnableOutput(true);
        s_output_enabled.store(true, std::memory_order_release);

        ESP_LOGI(TAG, "start MP3 playback");
        const esp_gmf_err_t result =
            esp_audio_simple_player_run_to_end(player, url.c_str(), nullptr);
        s_output_enabled.store(false, std::memory_order_release);

        bool is_current = false;
        {
            std::lock_guard<std::mutex> api_lock(s_api_mutex);
            {
                std::lock_guard<std::mutex> lock(s_player_mutex);
                if (s_player == player) {
                    s_player = nullptr;
                }
                is_current =
                    generation == s_generation.load(std::memory_order_relaxed);
            }
            esp_audio_simple_player_destroy(player);
        }

        if (is_current) {
            if (result == ESP_GMF_ERR_OK) {
                Emit(PodcastPlayerEvent::Finished, nullptr);
            } else {
                Emit(PodcastPlayerEvent::Error, "播放中断");
            }
        }
    }
    FinishPlayTask();
}

}  // namespace

bool PodcastPlayer::Start(const std::string& url, std::string& error) {
    error.clear();
    if (url.empty()) {
        error = "播放地址为空";
        return false;
    }

    std::lock_guard<std::mutex> api_lock(s_api_mutex);
    esp_asp_handle_t old_player = nullptr;
    bool create_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        const uint32_t generation =
            s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        old_player = s_player;
        s_player = nullptr;
        s_output_enabled.store(false, std::memory_order_release);

        auto* arg = new PlayArg{};
        arg->generation = generation;
        arg->url = url;
        if (xTaskCreate(PodcastPlayTask, "podcast_play", 8192, arg, 5,
                        &s_play_task) != pdPASS) {
            delete arg;
            s_play_task = nullptr;
            error = "创建播放任务失败";
            create_failed = true;
        }
    }
    if (old_player != nullptr) {
        esp_audio_simple_player_stop(old_player);
    }
    return !create_failed;
}

bool PodcastPlayer::Pause() {
    std::lock_guard<std::mutex> api_lock(s_api_mutex);
    esp_asp_handle_t player = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        player = s_player;
    }
    if (player == nullptr) {
        return false;
    }
    if (esp_audio_simple_player_pause(player) != ESP_GMF_ERR_OK) {
        return false;
    }
    s_output_enabled.store(false, std::memory_order_release);
    return true;
}

bool PodcastPlayer::Resume() {
    std::lock_guard<std::mutex> api_lock(s_api_mutex);
    esp_asp_handle_t player = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        player = s_player;
    }
    if (player == nullptr) {
        return false;
    }
    if (esp_audio_simple_player_resume(player) != ESP_GMF_ERR_OK) {
        return false;
    }
    s_output_enabled.store(true, std::memory_order_release);
    return true;
}

void PodcastPlayer::Stop() {
    std::lock_guard<std::mutex> api_lock(s_api_mutex);
    esp_asp_handle_t player = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        s_generation.fetch_add(1, std::memory_order_acq_rel);
        player = s_player;
        s_player = nullptr;
        s_output_enabled.store(false, std::memory_order_release);
    }
    if (player != nullptr) {
        esp_audio_simple_player_stop(player);
    }
}

void PodcastPlayer::SetEventCallback(PodcastPlayerEventCallback callback,
                                     void* ctx) {
    std::lock_guard<std::mutex> lock(s_player_mutex);
    s_event_cb = callback;
    s_event_ctx = ctx;
}
