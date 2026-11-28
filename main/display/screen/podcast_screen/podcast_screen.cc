#include "podcast_screen.h"
#include "utils/psram_task.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "application.h"
#include "board.h"
#include "chat_screen/chat_screen.h"
#include "dual_network_board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "home_screen/home_screen.h"
#include "http.h"
#include "i18n.h"
#include "podcast_client.h"
#include "podcast_player.h"
#include "podcast_types.h"
#include "SdCardManager.hpp"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

namespace {

constexpr const char* TAG = "PodcastScreen";

// 播客后台任务统一用 PSRAM 当栈：真机内部 RAM 只剩几 KB 时，
// 8~12KB 的栈会让 xTaskCreate 直接失败（表现为“创建播放请求失败”）。
// 任务创建统一走 utils/psram_task.h 的 CreatePsramTask()。
constexpr int32_t kPanelSize = 720;
constexpr int32_t kListX = 20;
constexpr int32_t kListY = 104;
constexpr int32_t kListWidth = 680;
constexpr int32_t kListHeight = 424;
constexpr uint32_t kColorBg = 0x0B1020;
constexpr uint32_t kColorBgBottom = 0x070B15;
constexpr uint32_t kColorPanel = 0x111827;
constexpr uint32_t kColorCard = 0x182033;
constexpr uint32_t kColorCardPressed = 0x24314D;
constexpr uint32_t kColorCardPlaying = 0x1B2A47;
constexpr uint32_t kColorAccent = 0x60A5FA;
constexpr uint32_t kColorAccentDeep = 0x2563EB;
constexpr uint32_t kColorText = 0xF8FAFC;
constexpr uint32_t kColorSubtle = 0x94A3B8;
constexpr uint32_t kColorDanger = 0xF87171;
constexpr uint32_t kColorDone = 0x4ADE80;
// 三档语速：0.8 慢速、1.0 原速、1.2 快速。
constexpr float kSpeedOptions[] = {0.8f, 1.0f, 1.2f};
constexpr size_t kSpeedOptionCount =
    sizeof(kSpeedOptions) / sizeof(kSpeedOptions[0]);

struct PodcastUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* pause_btn = nullptr;
    lv_obj_t* pause_label = nullptr;
    lv_obj_t* stop_btn = nullptr;
    lv_obj_t* progress = nullptr;
    lv_obj_t* time_label = nullptr;
    lv_obj_t* speed_btn[kSpeedOptionCount] = {};
    lv_obj_t* speed_label[kSpeedOptionCount] = {};
    lv_timer_t* progress_timer = nullptr;
};

struct LoadResult {
    uint32_t session = 0;
    uint32_t generation = 0;
    PodcastList items;
    std::string error;
};

struct ResolveResult {
    uint32_t session = 0;
    uint32_t request_generation = 0;
    std::string podcast_id;
    std::string title;
    int duration_seconds = 0;
    int64_t click_us = 0;
    std::string error;
};

struct PlayerUiEvent {
    uint32_t session = 0;
    PodcastPlayerEvent event = PodcastPlayerEvent::Error;
    std::string detail;
};

PodcastUi s_ui;
lv_obj_t* s_bound_screen = nullptr;
lv_obj_t* s_return_screen = nullptr;
PodcastList s_items;
// 上次成功拉到的节目列表：再次进入先本地渲染，后台只做刷新。
PodcastList s_cached_items;
time_t s_items_at = 0;
// 当前播放的节目，用于签名地址过期后失效缓存并重试一次。
std::string s_current_podcast_id;
size_t s_current_index = 0;
bool s_url_retried = false;
int64_t s_click_us = 0;
// 点击看门狗：超过 8 秒没出声就重试一次，避免用户对着"正在准备播放..."干等。
lv_timer_t* s_click_watchdog = nullptr;
bool s_click_retry_used = false;

constexpr const char* kListCachePath = "/sdcard/system/podcast_list.txt";
constexpr const char* kAudioCacheDir = "/sdcard/system/podcast";
constexpr size_t kMaxCachedEpisodes = 12;
constexpr int64_t kMaxAudioFileBytes = 96 * 1024 * 1024;

// 落盘在加载线程里做（不要再放回 LVGL 线程：写卡几十到几百毫秒会吃掉帧）。
void SaveListCacheItems(const PodcastList& items, time_t at) {
    FILE* file = std::fopen(kListCachePath, "w");
    if (file == nullptr) {
        return;
    }
    // newlib-nano 不支持 %lld，写出来会变成 "ld"，导致每次进页都被判为过期。
    std::fprintf(file, "#%d\n", static_cast<int>(at));
    for (const auto& item : items) {
        std::fprintf(file, "%s|%s|%d|%d\n", item.id.c_str(),
                     item.title.c_str(), item.duration_seconds,
                     item.completed ? 1 : 0);
    }
    std::fclose(file);
}

// 列表落盘后，冷启动第一次进入也能立刻看到节目。
void LoadListCache() {
    if (!s_cached_items.empty()) {
        return;
    }
    FILE* file = std::fopen(kListCachePath, "r");
    if (file == nullptr) {
        return;
    }
    PodcastList items;
    char line[512];
    bool has_timestamp = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string text(line);
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (!has_timestamp && !text.empty() && text.front() == '#') {
            const long long timestamp = std::atoll(text.c_str() + 1);
            if (timestamp > 0) {
                s_items_at = static_cast<time_t>(timestamp);
                has_timestamp = true;
            }
            continue;
        }
        const size_t first = text.find('|');
        if (first == std::string::npos) {
            continue;
        }
        const size_t second = text.find('|', first + 1);
        if (second == std::string::npos) {
            continue;
        }
        const size_t third = text.find('|', second + 1);
        if (third == std::string::npos) {
            continue;
        }
        PodcastItem item;
        item.id = text.substr(0, first);
        item.title = text.substr(first + 1, second - first - 1);
        item.duration_seconds = std::atoi(
            text.substr(second + 1, third - second - 1).c_str());
        item.completed = text.substr(third + 1) == "1";
        if (!item.id.empty()) {
            items.push_back(std::move(item));
        }
    }
    std::fclose(file);
    s_cached_items = std::move(items);
    if (!has_timestamp && !s_cached_items.empty()) {
        // 兼容旧缓存：先按刚写入处理，避免升级后首次进入又强制刷新。
        s_items_at = std::time(nullptr);
    }
}

void EnsureAudioCacheDir() {
    mkdir("/sdcard/system", 0775);
    mkdir(kAudioCacheDir, 0775);
}

std::string SanitizeCacheId(const std::string& podcast_id) {
    std::string safe;
    safe.reserve(podcast_id.size());
    for (char c : podcast_id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
            c == '_') {
            safe.push_back(c);
        }
    }
    if (safe.empty()) {
        safe = "podcast";
    }
    if (safe.size() > 96) {
        safe.resize(96);
    }
    return safe;
}

std::string AudioCacheExtension(const std::string& url,
                                const std::string& format) {
    auto normalize = [](const std::string& value) {
        std::string ext;
        for (char c : value) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                ext.push_back(
                    static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        return ext;
    };
    std::string ext = normalize(format);
    if (ext.size() >= 2 && ext.size() <= 5) {
        return "." + ext;
    }
    const std::string path = url.substr(0, url.find('?'));
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
        ext = normalize(path.substr(dot + 1));
        if (ext.size() >= 2 && ext.size() <= 5) {
            return "." + ext;
        }
    }
    return ".mp3";
}

std::string FindCachedAudio(const std::string& podcast_id) {
    const std::string prefix = SanitizeCacheId(podcast_id) + ".";
    DIR* dir = opendir(kAudioCacheDir);
    if (dir == nullptr) {
        return {};
    }
    std::string found;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() <= prefix.size() ||
            name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        if (name.size() >= 5 &&
            name.compare(name.size() - 5, 5, ".part") == 0) {
            continue;
        }
        found = std::string(kAudioCacheDir) + "/" + name;
        break;
    }
    closedir(dir);
    return found;
}

std::string s_current_title;
// 最近一次列表加载失败的原因；空列表时显示给用户，并配一个刷新按钮。
std::string s_last_error;
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_loading{false};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_request_generation{0};
std::atomic<bool> s_preparing_play{false};
std::atomic<uint32_t> s_prefetch_generation{0};
std::atomic<uint32_t> s_load_generation{0};
std::atomic<bool> s_download_running{false};
std::atomic<uint32_t> s_download_generation{0};
std::atomic<bool> s_playback_active{false};
bool s_playing = false;
int s_progress_max = 1;
bool s_paused = false;

void SetStatusDirect(const char* text);
void StartLoadWorker(bool manual_refresh = false);
void OnRefreshClicked(lv_event_t* event);
void ArmClickWatchdog();

uint32_t CurrentSession() {
    return s_session.load(std::memory_order_relaxed);
}

bool IsCurrentSession(uint32_t session) {
    return s_screen_active.load(std::memory_order_relaxed) &&
           session == CurrentSession();
}

bool DownloadAudioToCache(const std::string& url, const std::string& path,
                          uint32_t generation) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }
    const std::string part =
        path + "." + std::to_string(generation) + ".part";
    FILE* file = std::fopen(part.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    http->SetTimeout(20000);
    http->SetHeader("Connection", "close");
    bool ok = http->Open("GET", url);
    const int status = ok ? http->GetStatusCode() : 0;
    if (ok && (status < 200 || status >= 300)) {
        ok = false;
    }
    int64_t written = 0;
    char buffer[4096];
    while (ok) {
        if (generation !=
                s_download_generation.load(std::memory_order_acquire) ||
            !s_screen_active.load(std::memory_order_acquire)) {
            ok = false;
            break;
        }
        if (s_preparing_play.load(std::memory_order_acquire) ||
            s_playback_active.load(std::memory_order_acquire)) {
            ok = false;
            break;
        }
        const int read = http->Read(buffer, sizeof(buffer));
        if (read < 0) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        if (std::fwrite(buffer, 1, static_cast<size_t>(read), file) !=
            static_cast<size_t>(read)) {
            ok = false;
            break;
        }
        written += read;
        if (written > kMaxAudioFileBytes) {
            ok = false;
            break;
        }
    }
    const size_t expected = ok ? http->GetBodyLength() : 0;
    if (expected > 0 && static_cast<size_t>(written) != expected) {
        ok = false;
    }
    http->Close();
    std::fflush(file);
    fsync(fileno(file));
    std::fclose(file);
    if (!ok || written < 4096) {
        unlink(part.c_str());
        return false;
    }
    unlink(path.c_str());
    if (std::rename(part.c_str(), path.c_str()) != 0) {
        unlink(part.c_str());
        return false;
    }
    return true;
}

void PruneAudioCache(const std::string& keep_id) {
    struct CacheEntry {
        std::string path;
        time_t mtime = 0;
    };
    DIR* dir = opendir(kAudioCacheDir);
    if (dir == nullptr) {
        return;
    }
    std::vector<CacheEntry> entries;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.empty() || name == "." || name == "..") {
            continue;
        }
        if (name.size() >= 5 &&
            name.compare(name.size() - 5, 5, ".part") == 0) {
            continue;
        }
        const std::string path = std::string(kAudioCacheDir) + "/" + name;
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        entries.push_back({path, info.st_mtime});
    }
    closedir(dir);
    if (entries.size() <= kMaxCachedEpisodes) {
        return;
    }
    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) {
                  return a.mtime < b.mtime;
              });
    const std::string keep_prefix = SanitizeCacheId(keep_id) + ".";
    size_t excess = entries.size() - kMaxCachedEpisodes;
    for (const auto& entry : entries) {
        if (excess == 0) {
            break;
        }
        const size_t slash = entry.path.find_last_of('/');
        const std::string name =
            slash == std::string::npos ? entry.path
                                       : entry.path.substr(slash + 1);
        if (name.compare(0, keep_prefix.size(), keep_prefix) == 0) {
            continue;
        }
        if (unlink(entry.path.c_str()) == 0) {
            --excess;
        }
    }
}

struct AudioCacheTaskArg {
    uint32_t generation = 0;
    std::vector<std::string> ids;
};

void AudioCacheWorker(void* data) {
    std::unique_ptr<AudioCacheTaskArg> arg(
        static_cast<AudioCacheTaskArg*>(data));
    const uint32_t generation = arg->generation;
    EnsureAudioCacheDir();
    for (size_t i = 0; i < arg->ids.size();) {
        if (generation !=
                s_download_generation.load(std::memory_order_acquire) ||
            !s_screen_active.load(std::memory_order_acquire)) {
            break;
        }
        const std::string& id = arg->ids[i];
        if (!FindCachedAudio(id).empty()) {
            ++i;
            continue;
        }
        if (s_playback_active.load(std::memory_order_acquire) ||
            s_preparing_play.load(std::memory_order_acquire)) {
            while (s_playback_active.load(std::memory_order_acquire) ||
                   s_preparing_play.load(std::memory_order_acquire)) {
                if (generation !=
                        s_download_generation.load(
                            std::memory_order_acquire) ||
                    !s_screen_active.load(std::memory_order_acquire)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (generation !=
                    s_download_generation.load(
                        std::memory_order_acquire) ||
                !s_screen_active.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
        std::string error;
        const PodcastPlayInfo play = PodcastClient::ResolvePlayUrl(id, error);
        if (!play.ok) {
            ++i;
            continue;
        }
        if (generation !=
            s_download_generation.load(std::memory_order_acquire)) {
            break;
        }
        const std::string path = std::string(kAudioCacheDir) + "/" +
                                 SanitizeCacheId(id) +
                                 AudioCacheExtension(play.url, play.format);
        if (DownloadAudioToCache(play.url, path, generation)) {
            ESP_LOGI(TAG, "audio cached %s", path.c_str());
            PruneAudioCache(id);
            ++i;
        } else if (
            (s_playback_active.load(std::memory_order_acquire) ||
             s_preparing_play.load(std::memory_order_acquire)) &&
            generation ==
                s_download_generation.load(std::memory_order_acquire)) {
            // 播放抢占了带宽，这次下载已作废；等播放结束后重试同一条。
            continue;
        } else {
            ++i;
        }
    }
    if (generation ==
        s_download_generation.load(std::memory_order_acquire)) {
        s_download_running.store(false, std::memory_order_release);
    }
    DeleteCurrentPsramTask();
}

void StartAudioCacheWorker(size_t max_items = kMaxCachedEpisodes) {
    if (!s_screen_active.load(std::memory_order_acquire) ||
        s_items.empty()) {
        return;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual == nullptr || dual->GetNetworkType() != NetworkType::WIFI) {
        return;
    }
    if (s_download_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto* arg = new AudioCacheTaskArg{};
    const uint32_t generation = s_download_generation.fetch_add(1) + 1;
    arg->generation = generation;
    const size_t count =
        s_items.size() < max_items ? s_items.size() : max_items;
    arg->ids.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        arg->ids.push_back(s_items[i].id);
    }
    if (CreatePsramTask(AudioCacheWorker, "podcast_cache", 8192, arg, 2,
                    nullptr) != pdPASS) {
        delete arg;
        if (generation ==
            s_download_generation.load(std::memory_order_acquire)) {
            s_download_running.store(false, std::memory_order_release);
        }
    }
}

void FormatDuration(int seconds, char* buffer, size_t size) {
    if (seconds <= 0) {
        std::snprintf(buffer, size, "--:--");
        return;
    }
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const int secs = seconds % 60;
    if (hours > 0) {
        std::snprintf(buffer, size, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        std::snprintf(buffer, size, "%d:%02d", minutes, secs);
    }
}

void UpdateControls(bool visible, bool playing) {
    if (s_ui.pause_btn == nullptr || s_ui.stop_btn == nullptr ||
        s_ui.pause_label == nullptr) {
        return;
    }
    // 语速三档常驻可见：开始播放前也能先选好，播放中随时切换。
    lv_obj_t* extra[] = {s_ui.progress, s_ui.time_label};
    if (visible) {
        lv_obj_remove_flag(s_ui.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.stop_btn, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* obj : extra) {
            if (obj != nullptr) {
                lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_obj_add_flag(s_ui.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.stop_btn, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* obj : extra) {
            if (obj != nullptr) {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    lv_label_set_text(s_ui.pause_label,
                      I18n::T(playing ? "暂停" : "继续"));
}

void UpdateTimeLabel(int position, int duration) {
    if (s_ui.time_label == nullptr) {
        return;
    }
    char left[24] = {};
    char right[24] = {};
    if (position <= 0) {
        std::snprintf(left, sizeof(left), "0:00");
    } else {
        FormatDuration(position, left, sizeof(left));
    }
    FormatDuration(duration, right, sizeof(right));
    char text[56] = {};
    std::snprintf(text, sizeof(text), "%s / %s", left, right);
    lv_label_set_text(s_ui.time_label, text);
}

void OnProgressTimer(lv_timer_t* /*timer*/) {
    if (!s_screen_active.load(std::memory_order_relaxed) ||
        s_ui.progress == nullptr) {
        return;
    }
    // 预播阶段管线会提前解码，位置不能拿来推进度条。
    if (!s_playing && !s_paused) {
        return;
    }
    if (lv_obj_has_state(s_ui.progress, LV_STATE_PRESSED)) {
        return;  // 用户正在拖动，不覆盖滑块
    }
    const int duration = PodcastPlayer::GetDurationSeconds();
    const int position = PodcastPlayer::GetPositionSeconds();
    if (duration > 0 && s_progress_max != duration) {
        // duration_seconds 为 0 的节目会在开播后由比特率估算出量程。
        s_progress_max = duration;
        lv_slider_set_range(s_ui.progress, 0, duration);
    }
    lv_slider_set_value(s_ui.progress, position, LV_ANIM_OFF);
    UpdateTimeLabel(position, duration);
}

void OnProgressReleased(lv_event_t* event) {
    if (s_ui.progress == nullptr ||
        lv_event_get_target(event) != s_ui.progress) {
        return;
    }
    const int target = lv_slider_get_value(s_ui.progress);
    std::string error;
    if (!PodcastPlayer::Seek(target, error)) {
        SetStatusDirect(error.empty() ? I18n::T("暂不支持拖动")
                                      : error.c_str());
        return;
    }
    UpdateTimeLabel(target, PodcastPlayer::GetDurationSeconds());
    SetStatusDirect(I18n::T("正在跳转..."));
}

// 三档语速按钮的选中态：当前档位用实色强调，其余保持暗色。
void ApplySpeedUi() {
    const float speed = PodcastPlayer::GetSpeed();
    for (size_t i = 0; i < kSpeedOptionCount; ++i) {
        const bool active =
            speed > kSpeedOptions[i] - 0.001f && speed < kSpeedOptions[i] + 0.001f;
        if (s_ui.speed_btn[i] != nullptr) {
            lv_obj_set_style_bg_color(
                s_ui.speed_btn[i],
                lv_color_hex(active ? kColorAccentDeep : 0x1F2A3E), LV_PART_MAIN);
        }
        if (s_ui.speed_label[i] != nullptr) {
            lv_obj_set_style_text_color(
                s_ui.speed_label[i],
                lv_color_hex(active ? 0xFFFFFF : kColorSubtle), LV_PART_MAIN);
        }
    }
}

void OnSpeedClicked(lv_event_t* event) {
    const auto index = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (index >= kSpeedOptionCount) {
        return;
    }
    PodcastPlayer::SetSpeed(kSpeedOptions[index]);
    ApplySpeedUi();
}

void SetStatusDirect(const char* text) {
    if (s_ui.status != nullptr) {
        lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    }
}

lv_obj_t* CreateActionButton(lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                             int32_t h, const char* text,
                             lv_event_cb_t callback, void* user_data = nullptr) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, w, h);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x25324A),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x34466A),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(button, true);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void BeginPlayAt(size_t index) {
    if (index >= s_items.size()) {
        return;
    }
    // 点击即关 WiFi 省电：省电模式下首包要等 DTIM，实测"出声慢 + 卡顿"主要由它造成。
    // 播放结束时由 AudioService::SetExternalPlayback(false) 恢复。
    Board::GetInstance().SetPowerSaveMode(false);
    // 点击后立刻掐掉后台预取：它和这次建流抢 TLS 会话与内部 RAM，
    // 实测"点第一条要等十几秒、之后才突然出声"就是这种抢占造成的。
    s_prefetch_generation.fetch_add(1, std::memory_order_acq_rel);
    // 整集缓存也停掉：播放优先用带宽；已下完的文件仍然走本地播放。
    s_download_generation.fetch_add(1, std::memory_order_acq_rel);
    s_download_running.store(false, std::memory_order_release);
    const PodcastItem item = s_items[index];
    s_current_index = index;
    s_current_podcast_id = item.id;
    s_click_us = esp_timer_get_time();
    ESP_LOGI(TAG, "[timing] click id=%s index=%u", item.id.c_str(),
             static_cast<unsigned>(index));
    s_current_title = item.title;
    PodcastPlayer::SetDuration(item.duration_seconds);
    s_progress_max = item.duration_seconds > 0 ? item.duration_seconds : 1;
    if (s_ui.progress != nullptr) {
        lv_slider_set_range(s_ui.progress, 0, s_progress_max);
        lv_slider_set_value(s_ui.progress, 0, LV_ANIM_OFF);
    }
    UpdateTimeLabel(0, item.duration_seconds);
    const uint32_t request_generation =
        s_request_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t session = CurrentSession();
    // 别的节目在播就立刻停；只有“已预播未开始”的播放器要留给下面复用。
    PodcastPlayer::StopUnlessPrepared();
    s_playback_active.store(false, std::memory_order_release);
    s_playing = false;
    s_paused = false;
    s_preparing_play.store(true, std::memory_order_release);
    UpdateControls(false, false);
    // 准备阶段也留一个可点的“停止”，免得用户等半天却没法取消。
    if (s_ui.stop_btn != nullptr) {
        lv_obj_remove_flag(s_ui.stop_btn, LV_OBJ_FLAG_HIDDEN);
    }
    SetStatusDirect(I18n::T("正在准备播放..."));

    auto* context = new ResolveResult{};
    context->session = session;
    context->request_generation = request_generation;
    context->podcast_id = item.id;
    context->title = item.title;
    context->click_us = s_click_us;
    if (CreatePsramTask(
            [](void* arg) {
                auto* result = static_cast<ResolveResult*>(arg);
                std::string error;
                const std::string cached =
                    FindCachedAudio(result->podcast_id);
                if (result->session != CurrentSession() ||
                    result->request_generation !=
                        s_request_generation.load(
                            std::memory_order_relaxed)) {
                    result->error = "播放请求已取消";
                } else if (!cached.empty()) {
                    ESP_LOGI(TAG, "[timing] play local id=%s dt=%dms",
                             result->podcast_id.c_str(),
                             static_cast<int>((esp_timer_get_time() -
                                               result->click_us) /
                                              1000));
                    if (!PodcastPlayer::PlayPrepared(cached, error,
                                                     result->click_us)) {
                        PodcastPlayer::Stop();
                        if (!PodcastPlayer::Start(cached, error,
                                                  result->click_us)) {
                            result->error = error;
                        }
                    }
                } else {
                    const PodcastPlayInfo play =
                        PodcastClient::ResolvePlayUrl(result->podcast_id,
                                                     error);
                    ESP_LOGI(TAG, "[timing] screen resolve id=%s ok=%d dt=%dms",
                             result->podcast_id.c_str(), play.ok ? 1 : 0,
                             static_cast<int>((esp_timer_get_time() -
                                               result->click_us) /
                                              1000));
                    if (!play.ok) {
                        result->error =
                            play.error.empty() ? error : play.error;
                    } else if (PodcastPlayer::PlayPrepared(
                                   play.url, error, result->click_us)) {
                        // 进入列表时已预播：这里几乎立即出声。
                        ESP_LOGI(TAG, "[timing] screen prepared hit dt=%dms",
                                 static_cast<int>((esp_timer_get_time() -
                                                   result->click_us) /
                                                  1000));
                        result->duration_seconds = play.duration_seconds;
                    } else {
                        PodcastPlayer::Stop();
                        if (!PodcastPlayer::Start(play.url, error,
                                                  result->click_us)) {
                            result->error = error;
                        } else {
                            result->duration_seconds =
                                play.duration_seconds;
                        }
                    }
                }
                if (lv_async_call(
                    [](void* data) {
                        std::unique_ptr<ResolveResult> resolved(
                            static_cast<ResolveResult*>(data));
                        if (!IsCurrentSession(resolved->session)) {
                            return;
                        }
                        if (!resolved->error.empty() &&
                            resolved->error != "播放请求已取消") {
                            s_preparing_play.store(false,
                                                   std::memory_order_release);
                            s_playback_active.store(
                                false, std::memory_order_release);
                            SetStatusDirect(resolved->error.c_str());
                            UpdateControls(false, false);
                        } else if (resolved->duration_seconds > 0) {
                            PodcastPlayer::SetDuration(
                                resolved->duration_seconds);
                            s_progress_max = resolved->duration_seconds;
                            if (s_ui.progress != nullptr) {
                                lv_slider_set_range(s_ui.progress, 0,
                                                    s_progress_max);
                            }
                            UpdateTimeLabel(
                                PodcastPlayer::GetPositionSeconds(),
                                s_progress_max);
                        }
                    },
                    result) != LV_RESULT_OK) {
                    delete result;
                }
                DeleteCurrentPsramTask();
            },
            "podcast_play_url", 8192, context, 4, nullptr) != pdPASS) {
        delete context;
        SetStatusDirect(I18n::T("创建播放请求失败"));
    }
    ArmClickWatchdog();
}

// 点击看门狗：8 秒还没有出声迹象就失效缓存重试一次。
// 目的不是修网络，而是保证任何慢链路都有出口，不会停在无声的“正在准备播放...”。
void CancelClickWatchdog() {
    if (s_click_watchdog != nullptr) {
        lv_timer_delete(s_click_watchdog);
        s_click_watchdog = nullptr;
    }
}

void ClickWatchdogCb(lv_timer_t* timer) {
    lv_timer_delete(timer);
    s_click_watchdog = nullptr;
    if (!s_screen_active.load(std::memory_order_acquire) ||
        s_playback_active.load(std::memory_order_acquire) || s_playing) {
        return;
    }
    if (s_click_retry_used) {
        s_preparing_play.store(false, std::memory_order_release);
        SetStatusDirect(I18n::T("播放失败"));
        return;
    }
    s_click_retry_used = true;
    ESP_LOGW(TAG, "click watchdog: retry once id=%s",
             s_current_podcast_id.c_str());
    PodcastClient::InvalidatePlayUrl(s_current_podcast_id);
    SetStatusDirect(I18n::T("正在刷新播放地址..."));
    BeginPlayAt(s_current_index);
}

void ArmClickWatchdog() {
    CancelClickWatchdog();
    // 15 秒：正常建流 1~3 秒，留给慢链路余量；太早重试会把本来要成功的请求掐掉
    // （实测 8 秒重试会出现"先报播放失败、随后又自己响了"）。
    s_click_watchdog = lv_timer_create(ClickWatchdogCb, 15000, nullptr);
}

void OnPodcastClicked(lv_event_t* event) {
    const auto index = static_cast<size_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    s_url_retried = false;
    s_click_retry_used = false;
    CancelClickWatchdog();
    BeginPlayAt(index);
}

struct PrefetchTaskArg {
    uint32_t session = 0;
    uint32_t generation = 0;
    std::vector<std::string> ids;
};

// 预取播放地址（写进 play 缓存），并把第一集直接"预开流"：
// 用户停留在列表的这 1~2 秒里，TLS/HTTP/解码都已经就绪，
// 点第一集时只要把按住的首帧放出来，基本立刻出声。
void PrefetchFirstPlayUrlWorker(void* data) {
    std::unique_ptr<PrefetchTaskArg> arg(static_cast<PrefetchTaskArg*>(data));
    const uint32_t session = arg->session;
    const uint32_t generation = arg->generation;
    for (size_t i = 0; i < arg->ids.size(); ++i) {
        if (generation !=
                s_prefetch_generation.load(std::memory_order_acquire) ||
            !IsCurrentSession(session)) {
            break;
        }
        const std::string cached = FindCachedAudio(arg->ids[i]);
        if (!cached.empty()) {
            continue;
        }
        std::string error;
        const PodcastPlayInfo play =
            PodcastClient::ResolvePlayUrl(arg->ids[i], error);
        if (!play.ok) {
            continue;
        }
        ESP_LOGI(TAG, "prefetched play url #%u", static_cast<unsigned>(i));
        // 顺手把音频域名的 DNS 解掉：真机首次解析 1~3 秒，点击时再解析就是白等。
        PodcastClient::WarmUpHost(play.url);
        // 只处理第一集：多操作会和点击后的建流抢带宽，实测把第二集撞成建流超时。
        if (i != 0 || s_playing || s_paused ||
            s_preparing_play.load(std::memory_order_acquire) ||
            !IsCurrentSession(session)) {
            continue;
        }
        if (SdCardManager::GetInstance().IsMounted()) {
            // 有 SD 卡：后台按顺序把整集缓存到本地（最多 12 集，播放时自动让路）。
            // 缓存过的那一集点播走文件，出声约 0.2 秒——不管用户点第几集，
            // 只要它在后台已经轮到了就快。
            ESP_LOGI(TAG, "prefetch episodes to SD");
            StartAudioCacheWorker();
            continue;
        }
        std::string prepare_error;
        // 预开流会长期占住一条 TLS + 一块播放缓冲；内部 RAM 不宽裕时宁可不预开，
        // 也不能把别的模块挤到 CreateHttp 失败。
        const size_t largest_internal = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largest_internal < 96 * 1024) {
            ESP_LOGW(TAG, "skip prepare, internal largest=%u",
                     static_cast<unsigned>(largest_internal));
            continue;
        }
        if (PodcastPlayer::Prepare(play.url, prepare_error)) {
            ESP_LOGI(TAG, "prepare first episode started");
        } else {
            ESP_LOGW(TAG, "prepare skipped: %s", prepare_error.c_str());
        }
    }
    DeleteCurrentPsramTask();
}

void PrefetchFirstPlayUrl() {
    if (s_items.empty() || !s_screen_active.load(std::memory_order_acquire) ||
        s_playing || s_preparing_play.load(std::memory_order_acquire)) {
        return;
    }
    // 前 3 集都先取地址（串行）：点第 2、3 集时省掉 2~3 秒地址请求。
    constexpr size_t kPrefetchCount = 1;
    auto* arg = new PrefetchTaskArg{};
    arg->session = CurrentSession();
    arg->generation =
        s_prefetch_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const size_t count =
        s_items.size() < kPrefetchCount ? s_items.size() : kPrefetchCount;
    arg->ids.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        arg->ids.push_back(s_items[i].id);
    }
    if (CreatePsramTask(PrefetchFirstPlayUrlWorker, "podcast_prefetch", 12288, arg,
                    3, nullptr) != pdPASS) {
        delete arg;
    }
}

// 列表标题必须保持单行：按显示宽度截断（CJK 记 2 个单位），
// 否则标题折成两行会压到右下角的时长/完成状态。
std::string TruncateForWidth(const std::string& text, size_t max_units) {
    std::string out;
    size_t units = 0;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > text.size()) {
            len = text.size() - i;
        }
        const size_t glyph_units = len > 1 ? 2 : 1;
        if (units + glyph_units > max_units) {
            out += "…";
            break;
        }
        out.append(text, i, len);
        units += glyph_units;
        i += len;
    }
    return out;
}

lv_obj_t* CreatePillButton(lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                           int32_t h, const char* text,
                           lv_event_cb_t callback, void* user_data) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_radius(button, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x22304A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x34466A),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(button, true);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

// ---- 列表头像图标 ---------------------------------------------------------
// 数学=圆规、英语=地球、教育/家庭=大人带小孩，其余一律书本（讲书/读书类都适用）。
enum class PodcastAvatar { Book, Compass, Globe, Family };

PodcastAvatar AvatarForTitle(const std::string& title) {
    if (title.find("数学") != std::string::npos ||
        title.find("Math") != std::string::npos ||
        title.find("math") != std::string::npos) {
        return PodcastAvatar::Compass;
    }
    if (title.find("英语") != std::string::npos ||
        title.find("英文") != std::string::npos ||
        title.find("English") != std::string::npos ||
        title.find("english") != std::string::npos) {
        return PodcastAvatar::Globe;
    }
    if (title.find("教育") != std::string::npos ||
        title.find("家庭") != std::string::npos ||
        title.find("亲子") != std::string::npos) {
        return PodcastAvatar::Family;
    }
    return PodcastAvatar::Book;
}

void CreateFontAwesomeIcon(lv_obj_t* parent, const char* utf8) {
    lv_obj_t* icon = lv_label_create(parent);
    lv_label_set_text(icon, utf8);
    lv_obj_set_style_text_font(icon, &font_awesome_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(icon);
    screen_make_input_passive(icon);
}

// 书本：字体里没有书本图标，用两页白色纸面拼出来，中间留书脊缝。
void CreateBookIcon(lv_obj_t* parent) {
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* page = lv_obj_create(parent);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_size(page, 13, 21);
        lv_obj_set_pos(page, i == 0 ? 15 : 32, 20);
        lv_obj_set_style_radius(page, 3, LV_PART_MAIN);
        lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(page, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    }
}

// 家庭教育：字体里没有"大人拉小孩"，用一大一小两个人形拼出来。
void CreateFamilyIcon(lv_obj_t* parent) {
    lv_obj_t* adult = lv_label_create(parent);
    lv_label_set_text(adult, "\xEF\x80\x87");
    lv_obj_set_style_text_font(adult, &font_awesome_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(adult, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(adult, 5, 11);
    screen_make_input_passive(adult);

    lv_obj_t* child = lv_label_create(parent);
    lv_label_set_text(child, "\xEF\x80\x87");
    lv_obj_set_style_text_font(child, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(child, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(child, 33, 26);
    screen_make_input_passive(child);
}

// 空列表 / 加载失败：居中提示 + 刷新按钮，避免用户卡在“暂无节目”无路可走。
void BuildEmptyState() {
    lv_obj_t* box = lv_obj_create(s_ui.list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, kListWidth - 48, kListHeight - 48);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 12, LV_PART_MAIN);
    screen_make_input_passive(box);

    lv_obj_t* emblem = lv_label_create(box);
    lv_label_set_text(emblem, "Podcast");
    lv_obj_set_style_text_font(emblem, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(emblem, lv_color_hex(kColorAccent),
                                LV_PART_MAIN);
    screen_make_input_passive(emblem);

    lv_obj_t* hint = lv_label_create(box);
    lv_label_set_text(hint, I18n::T("暂无节目"));
    lv_obj_set_style_text_font(hint, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    screen_make_input_passive(hint);

    if (!s_last_error.empty()) {
        lv_obj_t* error = lv_label_create(box);
        lv_label_set_text(error, s_last_error.c_str());
        lv_label_set_long_mode(error, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(error, 520);
        lv_obj_set_style_text_font(error, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(error, lv_color_hex(kColorDanger),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(error, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        screen_make_input_passive(error);
    }

    CreatePillButton(box, 0, 0, 180, 56, I18n::T("刷新"), OnRefreshClicked,
                     nullptr);
}

void RebuildList() {
    if (s_ui.list == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list);

    if (s_items.empty()) {
        BuildEmptyState();
        return;
    }

    for (size_t index = 0; index < s_items.size(); ++index) {
        const PodcastItem& item = s_items[index];
        const bool is_current = !s_current_podcast_id.empty() &&
                                item.id == s_current_podcast_id &&
                                (s_playing || s_paused ||
                                 s_preparing_play.load(std::memory_order_acquire));
        lv_obj_t* row = lv_obj_create(s_ui.list);
        lv_obj_set_size(row, kListWidth - 24, 92);
        lv_obj_set_style_radius(row, 20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            row, lv_color_hex(is_current ? kColorCardPlaying : kColorCard),
            LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCardPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, is_current ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(kColorAccent),
                                      LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            row, OnPodcastClicked, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<uintptr_t>(index)));

        // 左侧字母头像：无封面图也能有稳定的视觉锚点。
        lv_obj_t* avatar = lv_obj_create(row);
        lv_obj_remove_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(avatar, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_pos(avatar, 16, 16);
        lv_obj_set_size(avatar, 60, 60);
        lv_obj_set_style_radius(avatar, 18, LV_PART_MAIN);
        lv_obj_set_style_border_width(avatar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(avatar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(kColorAccentDeep),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(
            avatar, lv_color_hex(is_current ? kColorAccent : 0x1E3A8A),
            LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(avatar, LV_GRAD_DIR_VER, LV_PART_MAIN);
        switch (AvatarForTitle(item.title)) {
            case PodcastAvatar::Compass:
                CreateFontAwesomeIcon(avatar, "\xEF\x85\x8E");  // 圆规
                break;
            case PodcastAvatar::Globe:
                CreateFontAwesomeIcon(avatar, "\xEF\x82\xAC");  // 地球
                break;
            case PodcastAvatar::Family:
                CreateFamilyIcon(avatar);
                break;
            case PodcastAvatar::Book:
                CreateBookIcon(avatar);
                break;
        }

        lv_obj_t* title = lv_label_create(row);
        const std::string title_text = TruncateForWidth(item.title, 40);
        lv_label_set_text(title, title_text.c_str());
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(title, 92, 14);
        lv_obj_set_width(title, 430);
        lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            title, lv_color_hex(is_current ? kColorAccent : kColorText),
            LV_PART_MAIN);
        screen_make_input_passive(title);

        char duration[24];
        FormatDuration(item.duration_seconds, duration, sizeof(duration));
        char meta_text[96] = {};
        std::snprintf(meta_text, sizeof(meta_text), "%s · %s", duration,
                      item.completed ? I18n::T("已完成")
                                     : I18n::T("未完成"));
        lv_obj_t* meta = lv_label_create(row);
        lv_label_set_text(meta, meta_text);
        // 时长/完成状态固定右下角，标题只占上面一行，互不覆盖。
        lv_obj_align(meta, LV_ALIGN_BOTTOM_RIGHT, -20, -12);
        lv_obj_set_style_text_font(meta, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            meta, lv_color_hex(item.completed ? kColorDone : kColorSubtle),
            LV_PART_MAIN);
        screen_make_input_passive(meta);

        if (is_current) {
            lv_obj_t* badge = lv_label_create(row);
            lv_label_set_text(badge, I18n::T("正在播放"));
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -20, 12);
            lv_obj_set_style_text_font(badge, &font_puhui_20_4,
                                       LV_PART_MAIN);
            lv_obj_set_style_text_color(badge, lv_color_hex(kColorAccent),
                                        LV_PART_MAIN);
            screen_make_input_passive(badge);
        }
    }
    // 列表刷新后后台预取第一条播放地址，点击时直接命中缓存。
    PrefetchFirstPlayUrl();
}

void ApplyLoadResult(void* data) {
    std::unique_ptr<LoadResult> result(static_cast<LoadResult*>(data));
    if (!IsCurrentSession(result->session) ||
        result->generation !=
            s_load_generation.load(std::memory_order_acquire)) {
        return;
    }
    s_loading.store(false, std::memory_order_release);
    if (!result->error.empty()) {
        // 后台刷新失败时保留已有列表，不打断正在看的节目。
        s_last_error = result->error;
        if (s_items.empty() && !s_cached_items.empty()) {
            // 连"上次的节目"都不显示会让人以为列表被清空了：先用缓存顶上。
            s_items = s_cached_items;
            RebuildList();
            SetStatusDirect(I18n::T("加载节目失败"));
            return;
        }
        if (!s_items.empty()) {
            SetStatusDirect(result->error.c_str());
            return;
        }
        // 列表本来就是空的：状态栏给结论，空态里给出原因和刷新按钮。
        SetStatusDirect(I18n::T("加载节目失败"));
        RebuildList();
        return;
    }
    s_last_error.clear();
    s_items = std::move(result->items);
    s_cached_items = s_items;
    s_items_at = std::time(nullptr);
    if (s_items.empty()) {
        SetStatusDirect(I18n::T("暂无节目"));
    } else if (!s_preparing_play.load(std::memory_order_acquire)) {
        SetStatusDirect(I18n::T("选择节目开始收听"));
    }
    RebuildList();
    // 不再在这里启动"整集音频缓存"：它会连续下载多集，和点击后的建流抢带宽，
    // 实测直接把第二集撞成"播放失败@aud_dec"（建流超时）。
    // 缓存留作后续"播放中顺手存"的独立开关，不在首播路径上。
}

void StartLoadWorker(bool manual_refresh) {
    s_loading.store(true, std::memory_order_release);
    const uint32_t session = CurrentSession();
    const uint32_t generation =
        s_load_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!s_playing && !s_preparing_play.load(std::memory_order_acquire)) {
        if (manual_refresh) {
            SetStatusDirect(I18n::T("正在刷新节目..."));
        } else if (s_items.empty()) {
            SetStatusDirect(I18n::T("正在加载节目..."));
        }
    }
    auto* context = new LoadResult{};
    context->session = session;
    context->generation = generation;
    if (CreatePsramTask(
            [](void* arg) {
                auto* result = static_cast<LoadResult*>(arg);
                std::string error;
                result->items = PodcastClient::FetchPodcasts(error);
                result->error = std::move(error);
                // 缓存写在加载线程里，避免 LVGL 线程同步写 SD 卡吃帧。
                if (result->error.empty() && !result->items.empty() &&
                    result->generation ==
                        s_load_generation.load(std::memory_order_acquire) &&
                    SdCardManager::GetInstance().IsMounted()) {
                    SaveListCacheItems(result->items, std::time(nullptr));
                }
                if (lv_async_call(ApplyLoadResult, result) != LV_RESULT_OK) {
                    if (result->generation ==
                        s_load_generation.load(
                            std::memory_order_acquire)) {
                        s_loading.store(false, std::memory_order_release);
                    }
                    delete result;
                }
                DeleteCurrentPsramTask();
            },
            "podcast_list", 8192, context, 4, nullptr) != pdPASS) {
        if (generation ==
            s_load_generation.load(std::memory_order_acquire)) {
            s_loading.store(false, std::memory_order_release);
        }
        delete context;
        SetStatusDirect(I18n::T("加载节目失败"));
    }
}

void ApplyPlayerEvent(void* data) {
    std::unique_ptr<PlayerUiEvent> event(
        static_cast<PlayerUiEvent*>(data));
    if (!IsCurrentSession(event->session)) {
        return;
    }
    // 已经有播放器状态回来，说明这条请求有结果了，看门狗不用再兜底。
    CancelClickWatchdog();
    switch (event->event) {
        case PodcastPlayerEvent::Playing:
            s_preparing_play.store(false, std::memory_order_release);
            s_playback_active.store(true, std::memory_order_release);
            s_playing = true;
            s_paused = false;
            UpdateControls(true, true);
            SetStatusDirect(s_current_title.empty()
                                ? I18n::T("正在播放")
                                : s_current_title.c_str());
            break;
        case PodcastPlayerEvent::Paused:
            s_preparing_play.store(false, std::memory_order_release);
            s_paused = true;
            UpdateControls(true, false);
            SetStatusDirect(I18n::T("已暂停"));
            break;
        case PodcastPlayerEvent::Finished:
            s_preparing_play.store(false, std::memory_order_release);
            s_playback_active.store(false, std::memory_order_release);
            s_playing = false;
            s_paused = false;
            UpdateControls(false, false);
            SetStatusDirect(I18n::T("播放结束"));
            break;
        case PodcastPlayerEvent::Error:
            s_preparing_play.store(false, std::memory_order_release);
            s_playback_active.store(false, std::memory_order_release);
            s_playing = false;
            s_paused = false;
            UpdateControls(false, false);
            SetStatusDirect(event->detail.empty()
                                ? I18n::T("播放失败")
                                : event->detail.c_str());
            break;
        case PodcastPlayerEvent::UrlExpired:
            if (!s_url_retried && !s_current_podcast_id.empty() &&
                s_current_index < s_items.size()) {
                s_url_retried = true;
                // 直连 /stream 被拒说明后端改了地址规则：关掉直连，回退到 /play。
                PodcastClient::ReportDirectStreamFailure();
                PodcastClient::InvalidatePlayUrl(s_current_podcast_id);
                ESP_LOGW(TAG, "signed url expired, retry once");
                SetStatusDirect(I18n::T("正在刷新播放地址..."));
                BeginPlayAt(s_current_index);
            } else {
                s_preparing_play.store(false, std::memory_order_release);
                s_playing = false;
                s_paused = false;
                UpdateControls(false, false);
                SetStatusDirect(I18n::T("播放地址已过期"));
            }
            break;
    }
    // 播放状态变化后同步列表高亮（“正在播放”/暂停/结束）。
    if (event->event != PodcastPlayerEvent::UrlExpired) {
        RebuildList();
    }
}

void OnPlayerEvent(PodcastPlayerEvent event, const char* detail,
                   void* /*ctx*/) {
    if (!s_screen_active.load(std::memory_order_relaxed)) {
        return;
    }
    auto* message = new PlayerUiEvent{};
    message->session = CurrentSession();
    message->event = event;
    if (detail != nullptr) {
        message->detail = detail;
    }
    if (lv_async_call(ApplyPlayerEvent, message) != LV_RESULT_OK) {
        delete message;
    }
}

void OnPauseClicked(lv_event_t* /*event*/) {
    if (s_playing && !s_paused) {
        if (PodcastPlayer::Pause()) {
            s_paused = true;
            UpdateControls(true, false);
            SetStatusDirect(I18n::T("已暂停"));
        }
    } else if (PodcastPlayer::Resume()) {
        s_paused = false;
        UpdateControls(true, true);
        SetStatusDirect(s_current_title.empty()
                            ? I18n::T("正在播放")
                            : s_current_title.c_str());
    }
}

void OnStopClicked(lv_event_t* /*event*/) {
    CancelClickWatchdog();
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_preparing_play.store(false, std::memory_order_release);
    s_playback_active.store(false, std::memory_order_release);
    PodcastPlayer::Stop();
    s_playing = false;
    s_paused = false;
    UpdateControls(false, false);
    SetStatusDirect(I18n::T("已停止"));
    RebuildList();
}

void ReturnHome() {
    const int64_t started_us = esp_timer_get_time();
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_prefetch_generation.fetch_add(1, std::memory_order_acq_rel);
    s_download_generation.fetch_add(1, std::memory_order_acq_rel);
    s_download_running.store(false, std::memory_order_release);
    s_preparing_play.store(false, std::memory_order_release);
    s_playback_active.store(false, std::memory_order_release);
    s_screen_active.store(false, std::memory_order_release);
    PodcastPlayer::Stop();
    lv_obj_t* old_screen = lv_screen_active();
    lv_obj_t* target = s_return_screen;
    s_return_screen = nullptr;
    if (target == nullptr || target == old_screen) {
        target = HomeScreen::Create();
    }
    lv_screen_load(target);
    if (target == ChatScreen::CurrentScreen()) {
        lv_timer_create(
            [](lv_timer_t* timer) {
                lv_timer_delete(timer);
                lv_timer_create(
                    [](lv_timer_t* retry_timer) {
                        lv_timer_delete(retry_timer);
                        ChatScreen::ResumeAfterTemporaryLeave();
                    },
                    300, nullptr);
            },
            1200, nullptr);
    }
    if (old_screen != nullptr && old_screen != target) {
        lv_obj_delete_async(old_screen);
    }
    ESP_LOGI(TAG, "[timing] return home dt=%dms",
             static_cast<int>((esp_timer_get_time() - started_us) / 1000));
}

// 播放中先停回节目列表，列表页再点才离开播客页。
void HandleBack() {
    CancelClickWatchdog();
    if (s_playing || s_paused) {
        s_preparing_play.store(false, std::memory_order_release);
        s_playback_active.store(false, std::memory_order_release);
        PodcastPlayer::Stop();
        s_playing = false;
        s_paused = false;
        s_current_title.clear();
        UpdateControls(false, false);
        RebuildList();
        SetStatusDirect(I18n::T("选择节目开始播放"));
        return;
    }
    ReturnHome();
}

void OnBackClicked(lv_event_t* /*event*/) {
    HandleBack();
}

void OnSwipeBack() {
    HandleBack();
}

void OnRefreshClicked(lv_event_t* /*event*/) {
    CancelClickWatchdog();
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_prefetch_generation.fetch_add(1, std::memory_order_acq_rel);
    s_download_generation.fetch_add(1, std::memory_order_acq_rel);
    s_load_generation.fetch_add(1, std::memory_order_acq_rel);
    s_download_running.store(false, std::memory_order_release);
    s_preparing_play.store(false, std::memory_order_release);
    s_playback_active.store(false, std::memory_order_release);
    s_playing = false;
    s_paused = false;
    PodcastPlayer::Stop();
    UpdateControls(false, false);

    // 不先清空列表：刷新失败时继续显示上一批节目，不再直接掉到“暂无节目”。
    // 列表接口本身有 30 分钟内存缓存，不强制失效的话“刷新”等于没刷新。
    PodcastClient::InvalidateListCache();
    PodcastClient::InvalidateAllPlayUrls();
    StartLoadWorker(true);
}
void OnScreenUnloaded(lv_event_t* event) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (target != s_bound_screen) {
        return;
    }
    s_bound_screen = nullptr;
    s_screen_active.store(false, std::memory_order_release);
    CancelClickWatchdog();
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_prefetch_generation.fetch_add(1, std::memory_order_acq_rel);
    s_download_generation.fetch_add(1, std::memory_order_acq_rel);
    s_download_running.store(false, std::memory_order_release);
    s_preparing_play.store(false, std::memory_order_release);
    s_playback_active.store(false, std::memory_order_release);
    PodcastPlayer::SetEventCallback(nullptr, nullptr);
    PodcastPlayer::Stop();
    s_current_title.clear();
    s_playing = false;
    s_paused = false;
    if (s_ui.progress_timer != nullptr) {
        lv_timer_delete(s_ui.progress_timer);
    }
    s_ui = PodcastUi{};
}

void BuildHeader(lv_obj_t* screen) {
    lv_obj_t* back = lv_button_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 72);
    lv_obj_set_pos(back, 16, 22);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(back, true);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, I18n::T("一丢播客"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    // 和返回按钮垂直居中对齐（直接写死 y 会因为字号行高不同而偏上）。
    lv_obj_align_to(title, back, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    screen_make_input_passive(title);

    lv_obj_t* refresh = lv_button_create(screen);
    lv_obj_set_size(refresh, 112, 52);
    lv_obj_set_pos(refresh, 588, 28);
    lv_obj_set_style_radius(refresh, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(0x25324A),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(0x34466A),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(refresh, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(refresh, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(refresh, true);
    lv_obj_add_event_cb(refresh, OnRefreshClicked, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t* refresh_label = lv_label_create(refresh);
    lv_label_set_text(refresh_label, I18n::T("刷新"));
    lv_obj_set_style_text_font(refresh_label, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(refresh_label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(refresh_label);
}

}  // namespace

lv_obj_t* PodcastScreen::Create(lv_obj_t* return_screen) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.screen = screen;
    s_return_screen = return_screen;

    BuildHeader(screen);

    lv_obj_t* list = lv_obj_create(screen);
    s_ui.list = list;
    lv_obj_set_pos(list, kListX, kListY);
    lv_obj_set_size(list, kListWidth, kListHeight);
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorPanel),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    s_ui.status = lv_label_create(screen);
    lv_label_set_text(s_ui.status, I18n::T("正在加载节目..."));
    lv_obj_set_width(s_ui.status, 672);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 530);
    screen_make_input_passive(s_ui.status);

    s_ui.progress = lv_slider_create(screen);
    lv_obj_set_size(s_ui.progress, 592, 10);
    lv_obj_set_pos(s_ui.progress, 64, 592);
    s_progress_max = 1;
    lv_slider_set_range(s_ui.progress, 0, 1);
    lv_slider_set_value(s_ui.progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(0x1E293B),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(kColorAccent),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(kColorText),
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_ui.progress, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ui.progress, 0, LV_PART_KNOB);
    screen_swipe_back_ignore(s_ui.progress, true);
    lv_obj_add_event_cb(s_ui.progress, OnProgressReleased, LV_EVENT_RELEASED,
                        nullptr);

    s_ui.time_label = lv_label_create(screen);
    lv_label_set_text(s_ui.time_label, "0:00 / --:--");
    lv_obj_set_style_text_font(s_ui.time_label, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.time_label, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_align(s_ui.time_label, LV_ALIGN_TOP_RIGHT, -64, 606);
    screen_make_input_passive(s_ui.time_label);

    // 语速三档：0.8x / 1.0x / 1.2x，点哪个就是哪个，不再循环切换。
    for (size_t i = 0; i < kSpeedOptionCount; ++i) {
        char text[16] = {};
        std::snprintf(text, sizeof(text), "%.1fx",
                      static_cast<double>(kSpeedOptions[i]));
        lv_obj_t* button = CreateActionButton(
            screen, 24 + static_cast<int32_t>(i) * 82, 648, 76, 48, text,
            OnSpeedClicked,
            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        s_ui.speed_btn[i] = button;
        s_ui.speed_label[i] = lv_obj_get_child(button, 0);
    }
    ApplySpeedUi();

    s_ui.progress_timer = lv_timer_create(OnProgressTimer, 500, nullptr);

    s_ui.pause_label = nullptr;
    s_ui.pause_btn = CreateActionButton(screen, 320, 648, 168, 48,
                                        I18n::T("暂停"), OnPauseClicked);
    s_ui.pause_label =
        lv_obj_get_child(s_ui.pause_btn, 0);
    // 暂停是播放中的主操作，用强调色，避免和右侧停止按钮混淆（尺寸保持原样）。
    lv_obj_set_style_bg_color(s_ui.pause_btn, lv_color_hex(kColorAccentDeep),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.pause_btn, lv_color_hex(0x3B82F6),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    s_ui.stop_btn = CreateActionButton(screen, 504, 648, 168, 48,
                                       I18n::T("停止"), OnStopClicked);
    // 不改尺寸，只把命中区向外放宽 5px：手指边缘点上去也算按到。
    lv_obj_set_ext_click_area(s_ui.pause_btn, 5);
    lv_obj_set_ext_click_area(s_ui.stop_btn, 5);
    UpdateControls(false, false);

    LoadListCache();
    if (!s_cached_items.empty()) {
        s_items = s_cached_items;
        RebuildList();
        SetStatusDirect(I18n::T("选择节目开始收听"));
    }

    s_bound_screen = screen;
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(screen, OnSwipeBack);
    return screen;
}

void PodcastScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: podcast_screen");
        Application::GetInstance().SetVoiceUiDesired(false);
        s_screen_active.store(true, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        s_loading.store(false, std::memory_order_release);
        s_playing = false;
        s_paused = false;
        s_preparing_play.store(false, std::memory_order_release);
        s_playback_active.store(false, std::memory_order_release);
        PodcastPlayer::SetEventCallback(OnPlayerEvent, nullptr);
        UpdateControls(false, false);
        if (!s_items.empty()) {
            RebuildList();
        }
        // 列表在 30 分钟内视为新鲜，进入页面不再强制刷新。
        if (s_items.empty() || s_items_at == 0 ||
            std::time(nullptr) - s_items_at > 30 * 60) {
            StartLoadWorker();
        } else {
            // 只预取第一集的播放地址；不要在这里启动"整集缓存"，
            // 那会连续下载多集，和点击后的建流抢带宽（实测把第二集撞成建流超时）。
            PrefetchFirstPlayUrl();
        }
    } else {
        ESP_LOGI(TAG, "unload: podcast_screen");
        const int64_t started_us = esp_timer_get_time();
        s_screen_active.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        s_prefetch_generation.fetch_add(1, std::memory_order_acq_rel);
        s_download_generation.fetch_add(1, std::memory_order_acq_rel);
        s_download_running.store(false, std::memory_order_release);
        s_playback_active.store(false, std::memory_order_release);
        PodcastPlayer::SetEventCallback(nullptr, nullptr);
        // 这里曾等 4 秒收尾，返回键直接卡住。播放器摘除是同步完成的，
        // 后台 stop 收尾由新一次点播的开始任务自己等；这里只给 200ms 让常见情况干净退出。
        PodcastPlayer::StopAndWait(200);
        ESP_LOGI(TAG, "[timing] unload done dt=%dms",
                 static_cast<int>((esp_timer_get_time() - started_us) / 1000));
    }
}
