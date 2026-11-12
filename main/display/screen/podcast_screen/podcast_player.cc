#include "podcast_player.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "audio_codec.h"
#include "board.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_element.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "podcast_client.h"

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
std::atomic<bool> s_play_busy{false};
std::atomic<uint32_t> s_output_generation{0};
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

void StopCurrentPlayback(bool wait_for_exit) {
    esp_asp_handle_t old_player = nullptr;
    TaskHandle_t old_task = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        s_generation.fetch_add(1, std::memory_order_acq_rel);
        old_player = s_player;
        s_player = nullptr;
        old_task = s_play_task;
        s_play_task = nullptr;
        s_output_enabled.store(false, std::memory_order_release);
        s_output_generation.store(
            s_generation.load(std::memory_order_relaxed),
            std::memory_order_release);
    }
    s_stream.stop.store(true, std::memory_order_release);
    if (old_player != nullptr) {
        esp_audio_simple_player_stop(old_player);
    }
    // 必须等旧 run_to_end 返回并 destroy，否则新歌和旧歌会同时 OutputData。
    if (wait_for_exit && old_task != nullptr) {
        for (int i = 0; i < 100 && s_play_busy.load(std::memory_order_relaxed);
             ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
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

// 与电台页一致：在 pool 上把重采样目标固定为 16kHz。
// 播放器建好 pipeline 后再查 "aud_rate_cvt" 往往查不到，
// 44.1kHz 的 mp3 会按原采样率输出，导致播放异常。
void ForcePoolRateCvt16k(esp_gmf_pool_handle_t pool) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    if (pool == nullptr) {
        return;
    }
    const void* it = nullptr;
    esp_gmf_element_handle_t el = nullptr;
    while (esp_gmf_pool_iterate_element(pool, &it, &el) == ESP_GMF_ERR_OK) {
        if (el == nullptr) {
            continue;
        }
        const char* tag = OBJ_GET_TAG(el);
        if (tag == nullptr || strcmp(tag, "aud_rate_cvt") != 0) {
            continue;
        }
        esp_gmf_rate_cvt_set_dest_rate(el,
                                       static_cast<uint32_t>(kOutputSampleRate));
        auto* cfg = static_cast<esp_ae_rate_cvt_cfg_t*>(OBJ_GET_CFG(el));
        if (cfg != nullptr) {
            cfg->dest_rate = static_cast<uint32_t>(kOutputSampleRate);
        }
    }
#else
    (void)pool;
#endif
}

// 不走 GMF 的 HTTP IO：播客是 https 签名地址，自带证书的 IO 与播放器池里
// 默认的 "io_http" 同名，容易被换成不带证书的那个而握手失败。
// 这里改用 raw:// 输入，由本文件用 esp_http_client 直接拉流。
// 音频先由独立任务预取到 PSRAM 环形缓冲，播放回调只从内存取数据，
// 网络抖动不再直接变成断音。
constexpr int kRingBlockSize = 16 * 1024;
constexpr int kRingBlockCount = 96;  // 约 1.5MB
constexpr size_t kRingBytes =
    static_cast<size_t>(kRingBlockSize) * kRingBlockCount;

struct RingBlock {
    uint16_t index;
    uint16_t size;
};

struct StreamState {
    std::string url;
    std::string error;
    std::atomic<bool> stop{false};
    std::atomic<bool> ended{false};
    std::atomic<bool> open_ok{false};
    SemaphoreHandle_t opened = nullptr;
    TaskHandle_t task = nullptr;
    int pending_index = -1;
    int pending_size = 0;
    int pending_offset = 0;
};

uint8_t* s_ring = nullptr;
QueueHandle_t s_ring_free = nullptr;
QueueHandle_t s_ring_data = nullptr;
StreamState s_stream;

// open/fetch_headers 这套手动流程不会自动跟随 3xx，需要自己解析 Location。
std::string ResolveLocation(const std::string& base, const std::string& location) {
    if (location.rfind("http://", 0) == 0 ||
        location.rfind("https://", 0) == 0) {
        return location;
    }
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) {
        return "";
    }
    const size_t host_end = base.find('/', scheme_end + 3);
    const std::string origin =
        host_end == std::string::npos ? base : base.substr(0, host_end);
    if (!location.empty() && location[0] == '/') {
        return origin + location;
    }
    return "";
}

bool OpenAudioStream(const std::string& url, esp_http_client_handle_t& client,
                     std::string& error, int64_t range_start = 0,
                     int64_t* remaining = nullptr) {
    client = nullptr;
    std::string current = url;
    for (int hop = 0; hop < 4; ++hop) {
        esp_http_client_config_t cfg = {};
        cfg.url = current.c_str();
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.timeout_ms = 15000;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 1024;
        client = esp_http_client_init(&cfg);
        if (client == nullptr) {
            error = "创建网络连接失败";
            return false;
        }
        const std::string device_token = PodcastClient::DeviceToken();
        if (!device_token.empty()) {
            const std::string authorization = "Bearer " + device_token;
            esp_http_client_set_header(client, "Authorization",
                                       authorization.c_str());
        }
        if (range_start > 0) {
            char range[40] = {};
            snprintf(range, sizeof(range), "bytes=%lld-",
                     static_cast<long long>(range_start));
            esp_http_client_set_header(client, "Range", range);
        }
        if (esp_http_client_open(client, 0) != ESP_OK) {
            char detail[48] = {};
            snprintf(detail, sizeof(detail), "连接音频失败 %d",
                     esp_http_client_get_errno(client));
            error = detail;
            esp_http_client_cleanup(client);
            client = nullptr;
            return false;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            char* location = nullptr;
            esp_http_client_get_header(client, "Location", &location);
            const std::string next =
                location == nullptr ? "" : ResolveLocation(current, location);
            ESP_LOGI(TAG, "audio redirect %d -> %s", status,
                     next.empty() ? "(none)" : next.c_str());
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            if (next.empty()) {
                error = "音频地址跳转失败";
                return false;
            }
            current = next;
            continue;
        }
        if (status != 200 && status != 206) {
            char detail[40] = {};
            snprintf(detail, sizeof(detail), "音频 HTTP %d", status);
            error = detail;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            return false;
        }
        // 续传请求必须拿到 206，否则会从头重放，直接放弃续传。
        if (range_start > 0 && status != 206) {
            error = "音频续传失败";
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            return false;
        }
        if (remaining != nullptr) {
            *remaining =
                static_cast<int64_t>(esp_http_client_get_content_length(client));
        }
        ESP_LOGI(TAG, "audio stream open, status=%d len=%d", status,
                 static_cast<int>(esp_http_client_get_content_length(client)));
        return true;
    }
    error = "音频地址跳转过多";
    return false;
}

bool PrepareStreamBuffers() {
    if (s_ring == nullptr) {
        s_ring = static_cast<uint8_t*>(
            heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_ring == nullptr) {
        return false;
    }
    if (s_ring_free == nullptr) {
        s_ring_free = xQueueCreate(kRingBlockCount, sizeof(uint16_t));
    }
    if (s_ring_data == nullptr) {
        s_ring_data = xQueueCreate(kRingBlockCount, sizeof(RingBlock));
    }
    if (s_ring_free == nullptr || s_ring_data == nullptr) {
        return false;
    }
    uint16_t index = 0;
    RingBlock block = {};
    while (xQueueReceive(s_ring_free, &index, 0) == pdTRUE) {
    }
    while (xQueueReceive(s_ring_data, &block, 0) == pdTRUE) {
    }
    for (int i = 0; i < kRingBlockCount; ++i) {
        const uint16_t value = static_cast<uint16_t>(i);
        xQueueSend(s_ring_free, &value, 0);
    }
    return true;
}

void PodcastFetchTask(void* arg) {
    auto* state = static_cast<StreamState*>(arg);
    esp_http_client_handle_t client = nullptr;
    int64_t remaining = -1;
    std::string error;
    if (!OpenAudioStream(state->url, client, error, 0, &remaining)) {
        state->error = error;
        state->ended.store(true);
        xSemaphoreGive(state->opened);
        state->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    state->open_ok.store(true);
    xSemaphoreGive(state->opened);

    int64_t fetched = 0;
    int resumes = 0;
    while (!state->stop.load(std::memory_order_relaxed)) {
        uint16_t index = 0;
        if (xQueueReceive(s_ring_free, &index, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        uint8_t* dst = s_ring + static_cast<size_t>(index) * kRingBlockSize;
        int got = 0;
        bool stream_end = false;
        while (got < kRingBlockSize &&
               !state->stop.load(std::memory_order_relaxed)) {
            const int ret = esp_http_client_read(
                client, reinterpret_cast<char*>(dst) + got,
                kRingBlockSize - got);
            if (ret > 0) {
                got += ret;
                fetched += ret;
                if (remaining > 0) {
                    remaining -= ret;
                }
                continue;
            }
            // 长节目经代理播放时，连接可能被中途断开；按已读字节续传。
            if (remaining > 0 && resumes < 8) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                client = nullptr;
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_http_client_handle_t next = nullptr;
                int64_t next_remaining = -1;
                std::string resume_error;
                if (OpenAudioStream(state->url, next, resume_error, fetched,
                                    &next_remaining)) {
                    client = next;
                    remaining = next_remaining;
                    resumes++;
                    ESP_LOGW(TAG, "audio resumed at %lld (try %d)",
                             static_cast<long long>(fetched), resumes);
                    continue;
                }
                ESP_LOGW(TAG, "audio resume failed: %s",
                         resume_error.c_str());
            }
            stream_end = true;
            break;
        }
        if (got > 0 && !state->stop.load(std::memory_order_relaxed)) {
            RingBlock block = {index, static_cast<uint16_t>(got)};
            if (xQueueSend(s_ring_data, &block, pdMS_TO_TICKS(2000)) !=
                pdTRUE) {
                xQueueSend(s_ring_free, &index, 0);
            }
        } else {
            xQueueSend(s_ring_free, &index, 0);
        }
        if (stream_end) {
            break;
        }
    }
    if (client != nullptr) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    state->ended.store(true);
    state->task = nullptr;
    vTaskDelete(nullptr);
}

// 播放器要求每次尽量填满请求长度，短读会被当成流结束。
int PodcastReadCallback(uint8_t* data, int data_size, void* ctx) {
    auto* state = static_cast<StreamState*>(ctx);
    if (state == nullptr || data == nullptr || data_size <= 0) {
        return 0;
    }
    int total = 0;
    while (total < data_size) {
        if (state->pending_size <= 0) {
            RingBlock block = {};
            if (xQueueReceive(s_ring_data, &block,
                              pdMS_TO_TICKS(500)) != pdTRUE) {
                if (state->stop.load(std::memory_order_relaxed) ||
                    state->ended.load(std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }
            state->pending_index = block.index;
            state->pending_size = block.size;
            state->pending_offset = 0;
        }
        int take = state->pending_size - state->pending_offset;
        if (take > data_size - total) {
            take = data_size - total;
        }
        std::memcpy(data + total,
                    s_ring + static_cast<size_t>(state->pending_index) *
                                 kRingBlockSize + state->pending_offset,
                    static_cast<size_t>(take));
        total += take;
        state->pending_offset += take;
        if (state->pending_offset >= state->pending_size) {
            const uint16_t index =
                static_cast<uint16_t>(state->pending_index);
            xQueueSend(s_ring_free, &index, 0);
            state->pending_size = 0;
            state->pending_index = -1;
        }
    }
    return total;
}

int PodcastOutCallback(uint8_t* data, int data_size, void* ctx) {
    if (!s_output_enabled.load(std::memory_order_acquire) ||
        s_output_generation.load(std::memory_order_relaxed) !=
            s_generation.load(std::memory_order_relaxed) ||
        data == nullptr || data_size <= 0) {
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

int PodcastEventCallback(esp_asp_event_pkt_t* event, void* ctx) {
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
            // 把报错的环节带到界面上，便于区分网络/解码/重采样。
            const char* stage = "pipe";
            esp_gmf_pipeline_handle_t pipe = nullptr;
            if (ctx != nullptr &&
                esp_audio_simple_player_get_pipeline(
                    static_cast<esp_asp_handle_t>(ctx), &pipe) ==
                    ESP_GMF_ERR_OK &&
                pipe != nullptr) {
                static const char* const kNames[] = {"aud_dec",
                                                     "aud_rate_cvt"};
                for (const char* name : kNames) {
                    esp_gmf_element_handle_t el = nullptr;
                    if (esp_gmf_pipeline_get_el_by_name(pipe, name, &el) !=
                            ESP_GMF_ERR_OK ||
                        el == nullptr) {
                        continue;
                    }
                    esp_gmf_event_state_t el_state = ESP_GMF_EVENT_STATE_NONE;
                    if (esp_gmf_element_get_state(el, &el_state) ==
                            ESP_GMF_ERR_OK &&
                        el_state == ESP_GMF_EVENT_STATE_ERROR) {
                        stage = name;
                    }
                }
            }
            char detail[48] = {};
            snprintf(detail, sizeof(detail), "播放失败@%s", stage);
            Emit(PodcastPlayerEvent::Error, detail);
        }
    }
    return 0;
}

void PodcastPlayTask(void* arg) {
    auto* play_arg = static_cast<PlayArg*>(arg);
    const uint32_t generation = play_arg->generation;
    const std::string url = play_arg->url;
    delete play_arg;

    if (generation != s_generation.load(std::memory_order_acquire)) {
        FinishPlayTask();
        return;
    }
    s_play_busy.store(true, std::memory_order_relaxed);

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        Emit(PodcastPlayerEvent::Error, "音频输出不可用");
        s_play_busy.store(false, std::memory_order_relaxed);
        FinishPlayTask();
        return;
    }

    EnsureMediaLibAdapter();
    s_pcm_channels.store(2, std::memory_order_relaxed);
    s_reported_playing.store(false, std::memory_order_relaxed);

    // 上一轮预取任务可能还在收尾，先让它退出再复用缓冲。
    for (int i = 0; i < 50 && s_stream.task != nullptr; ++i) {
        s_stream.stop.store(true, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!PrepareStreamBuffers()) {
        Emit(PodcastPlayerEvent::Error, "音频缓冲不足");
        s_play_busy.store(false, std::memory_order_relaxed);
        FinishPlayTask();
        return;
    }
    s_stream.url = url;
    s_stream.error.clear();
    s_stream.stop.store(false, std::memory_order_release);
    s_stream.ended.store(false, std::memory_order_release);
    s_stream.open_ok.store(false, std::memory_order_release);
    s_stream.pending_index = -1;
    s_stream.pending_size = 0;
    s_stream.pending_offset = 0;
    if (s_stream.opened == nullptr) {
        s_stream.opened = xSemaphoreCreateBinary();
    }
    if (s_stream.opened != nullptr) {
        while (xSemaphoreTake(s_stream.opened, 0) == pdTRUE) {
        }
    }
    if (s_stream.opened == nullptr ||
        xTaskCreate(PodcastFetchTask, "podcast_fetch", 6144, &s_stream, 4,
                    &s_stream.task) != pdPASS) {
        Emit(PodcastPlayerEvent::Error, "创建预取任务失败");
        s_play_busy.store(false, std::memory_order_relaxed);
        FinishPlayTask();
        return;
    }
    if (xSemaphoreTake(s_stream.opened, pdMS_TO_TICKS(20000)) != pdTRUE ||
        !s_stream.open_ok.load(std::memory_order_relaxed)) {
        s_stream.stop.store(true, std::memory_order_release);
        s_stream.task = nullptr;
        const std::string detail =
            s_stream.error.empty() ? "连接音频超时" : s_stream.error;
        Emit(PodcastPlayerEvent::Error, detail.c_str());
        s_play_busy.store(false, std::memory_order_relaxed);
        FinishPlayTask();
        return;
    }

    {
        esp_asp_cfg_t cfg = {
            .in = {.cb = PodcastReadCallback, .user_ctx = &s_stream},
            .out = {.cb = PodcastOutCallback, .user_ctx = codec},
            .task_prio = 5,
            .task_stack = 8 * 1024,
            .prev = nullptr,
            .prev_ctx = nullptr,
        };
        esp_asp_handle_t player = nullptr;
        if (esp_audio_simple_player_new(&cfg, &player) != ESP_GMF_ERR_OK ||
            player == nullptr) {
            s_stream.stop.store(true, std::memory_order_release);
            Emit(PodcastPlayerEvent::Error, "创建播放器失败");
            s_play_busy.store(false, std::memory_order_relaxed);
            FinishPlayTask();
            return;
        }
        esp_gmf_pool_handle_t pool = nullptr;
        if (esp_audio_simple_player_get_pool(player, &pool) ==
                ESP_GMF_ERR_OK &&
            pool != nullptr) {
            ForcePoolRateCvt16k(pool);
        }
        ApplyRateCvt16k(player);
        esp_audio_simple_player_set_event(player, PodcastEventCallback,
                                          player);

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
            s_stream.stop.store(true, std::memory_order_release);
            s_play_busy.store(false, std::memory_order_relaxed);
            FinishPlayTask();
            return;
        }

        codec->EnableOutput(true);
        s_output_generation.store(generation, std::memory_order_relaxed);
        s_output_enabled.store(true, std::memory_order_release);

        ESP_LOGI(TAG, "start MP3 playback");
        const esp_gmf_err_t result =
            esp_audio_simple_player_run_to_end(player, "raw://podcast.mp3",
                                               nullptr);
        s_output_enabled.store(false, std::memory_order_release);
        s_stream.stop.store(true, std::memory_order_release);

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
                char detail[48] = {};
                snprintf(detail, sizeof(detail), "播放失败 0x%X",
                         static_cast<unsigned>(result));
                ESP_LOGE(TAG, "play failed, ret=0x%x url_len=%u",
                         static_cast<unsigned>(result),
                         static_cast<unsigned>(url.size()));
                Emit(PodcastPlayerEvent::Error, detail);
            }
        }
    }
    s_play_busy.store(false, std::memory_order_relaxed);
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
    StopCurrentPlayback(true);
    bool create_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        const uint32_t generation =
            s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
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
    StopCurrentPlayback(true);
}

void PodcastPlayer::SetEventCallback(PodcastPlayerEventCallback callback,
                                     void* ctx) {
    std::lock_guard<std::mutex> lock(s_player_mutex);
    s_event_cb = callback;
    s_event_ctx = ctx;
}
