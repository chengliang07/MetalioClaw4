#include "podcast_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "application.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "podcast_client.h"
#include "podcast_player.h"
#include "podcast_types.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "PodcastScreen";
constexpr int32_t kPanelSize = 720;
constexpr int32_t kHeaderHeight = 84;
constexpr int32_t kListX = 24;
constexpr int32_t kListY = 102;
constexpr int32_t kListWidth = 672;
constexpr int32_t kListHeight = 442;
constexpr uint32_t kColorBg = 0x0B1020;
constexpr uint32_t kColorPanel = 0x111827;
constexpr uint32_t kColorCard = 0x182033;
constexpr uint32_t kColorCardPressed = 0x24314D;
constexpr uint32_t kColorAccent = 0x60A5FA;
constexpr uint32_t kColorText = 0xF8FAFC;
constexpr uint32_t kColorSubtle = 0x94A3B8;

struct PodcastUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* pause_btn = nullptr;
    lv_obj_t* pause_label = nullptr;
    lv_obj_t* stop_btn = nullptr;
};

struct LoadResult {
    uint32_t session = 0;
    PodcastList items;
    std::string error;
};

struct ResolveResult {
    uint32_t session = 0;
    uint32_t request_generation = 0;
    std::string podcast_id;
    std::string title;
    std::string error;
};

struct PlayerUiEvent {
    uint32_t session = 0;
    PodcastPlayerEvent event = PodcastPlayerEvent::Error;
    std::string detail;
};

PodcastUi s_ui;
lv_obj_t* s_bound_screen = nullptr;
PodcastList s_items;
std::string s_current_title;
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_loading{false};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_request_generation{0};
bool s_playing = false;
bool s_paused = false;

void SetStatusDirect(const char* text);

uint32_t CurrentSession() {
    return s_session.load(std::memory_order_relaxed);
}

bool IsCurrentSession(uint32_t session) {
    return s_screen_active.load(std::memory_order_relaxed) &&
           session == CurrentSession();
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
    if (visible) {
        lv_obj_remove_flag(s_ui.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.stop_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.stop_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_ui.pause_label,
                      I18n::T(playing ? "暂停" : "继续"));
}

void SetStatusDirect(const char* text) {
    if (s_ui.status != nullptr) {
        lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    }
}

lv_obj_t* CreateActionButton(lv_obj_t* parent, int32_t x, const char* text,
                             lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, 150, 56);
    lv_obj_set_pos(button, x, 628);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x25324A),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x34466A),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(button, true);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void OnPodcastClicked(lv_event_t* event) {
    const auto index = static_cast<size_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    if (index >= s_items.size()) {
        return;
    }
    const PodcastItem item = s_items[index];
    s_current_title = item.title;
    const uint32_t request_generation =
        s_request_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t session = CurrentSession();
    PodcastPlayer::Stop();
    s_playing = false;
    s_paused = false;
    UpdateControls(false, false);
    SetStatusDirect(I18n::T("正在获取播放地址..."));

    auto* context = new ResolveResult{};
    context->session = session;
    context->request_generation = request_generation;
    context->podcast_id = item.id;
    context->title = item.title;
    if (xTaskCreate(
            [](void* arg) {
                auto* result = static_cast<ResolveResult*>(arg);
                std::string error;
                const PodcastPlayInfo play =
                    PodcastClient::ResolvePlayUrl(result->podcast_id, error);
                if (!play.ok) {
                    result->error =
                        play.error.empty() ? error : play.error;
                } else if (
                    result->session != CurrentSession() ||
                    result->request_generation !=
                        s_request_generation.load(
                            std::memory_order_relaxed)) {
                    result->error = "播放请求已取消";
                } else if (!PodcastPlayer::Start(play.url, error)) {
                    result->error = error;
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
                            SetStatusDirect(resolved->error.c_str());
                            UpdateControls(false, false);
                        }
                    },
                    result) != LV_RESULT_OK) {
                    delete result;
                }
                vTaskDelete(nullptr);
            },
            "podcast_play_url", 8192, context, 4, nullptr) != pdPASS) {
        delete context;
        SetStatusDirect(I18n::T("创建播放请求失败"));
    }
}

void RebuildList() {
    if (s_ui.list == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list);

    if (s_items.empty()) {
        lv_obj_t* hint = lv_label_create(s_ui.list);
        lv_label_set_text(hint, I18n::T("暂无节目"));
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_width(hint, kListWidth - 32);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        screen_make_input_passive(hint);
        return;
    }

    for (size_t index = 0; index < s_items.size(); ++index) {
        const PodcastItem& item = s_items[index];
        lv_obj_t* row = lv_obj_create(s_ui.list);
        lv_obj_set_size(row, kListWidth - 8, 88);
        lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCardPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            row, OnPodcastClicked, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<uintptr_t>(index)));

        lv_obj_t* title = lv_label_create(row);
        lv_label_set_text(title, item.title.c_str());
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, 470);
        lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
        screen_make_input_passive(title);

        char duration[24];
        FormatDuration(item.duration_seconds, duration, sizeof(duration));
        lv_obj_t* meta = lv_label_create(row);
        lv_label_set_text(meta, item.completed
                                    ? I18n::T("已完成")
                                    : I18n::T("未完成"));
        lv_obj_set_style_text_font(meta, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            meta, lv_color_hex(item.completed ? 0x4ADE80 : kColorSubtle),
            LV_PART_MAIN);
        lv_obj_align(meta, LV_ALIGN_TOP_RIGHT, 0, 0);
        screen_make_input_passive(meta);

        lv_obj_t* duration_label = lv_label_create(row);
        lv_label_set_text(duration_label, duration);
        lv_obj_set_style_text_font(duration_label, &font_puhui_20_4,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(duration_label,
                                    lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_align(duration_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        screen_make_input_passive(duration_label);
    }
}

void ApplyLoadResult(void* data) {
    std::unique_ptr<LoadResult> result(static_cast<LoadResult*>(data));
    if (!IsCurrentSession(result->session)) {
        return;
    }
    s_loading.store(false, std::memory_order_release);
    if (!result->error.empty()) {
        // 后台刷新失败时保留已有列表，不打断正在看的节目。
        if (s_items.empty()) {
            SetStatusDirect(result->error.c_str());
        }
        return;
    }
    s_items = std::move(result->items);
    if (s_items.empty()) {
        SetStatusDirect(I18n::T("暂无节目"));
    } else {
        SetStatusDirect(I18n::T("选择节目开始收听"));
    }
    RebuildList();
}

void StartLoadWorker() {
    bool expected = false;
    if (!s_loading.compare_exchange_strong(expected, true)) {
        return;
    }
    const uint32_t session = CurrentSession();
    if (s_items.empty()) {
        SetStatusDirect(I18n::T("正在加载节目..."));
    }
    auto* context = new LoadResult{};
    context->session = session;
    if (xTaskCreate(
            [](void* arg) {
                auto* result = static_cast<LoadResult*>(arg);
                std::string error;
                result->items = PodcastClient::FetchPodcasts(error);
                result->error = std::move(error);
                std::vector<std::string> warm_ids;
                for (size_t i = 0;
                     i < result->items.size() && i < 3; ++i) {
                    warm_ids.push_back(result->items[i].id);
                }
                if (lv_async_call(ApplyLoadResult, result) != LV_RESULT_OK) {
                    s_loading.store(false, std::memory_order_release);
                    delete result;
                }
                // 列表出来后在后台把前几个节目的播放地址取好，
                // 用户点开时不必再等一次请求。
                for (const auto& id : warm_ids) {
                    std::string warm_error;
                    PodcastClient::ResolvePlayUrl(id, warm_error);
                }
                vTaskDelete(nullptr);
            },
            "podcast_list", 8192, context, 4, nullptr) != pdPASS) {
        s_loading.store(false, std::memory_order_release);
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
    switch (event->event) {
        case PodcastPlayerEvent::Playing:
            s_playing = true;
            s_paused = false;
            UpdateControls(true, true);
            SetStatusDirect(s_current_title.empty()
                                ? I18n::T("正在播放")
                                : s_current_title.c_str());
            break;
        case PodcastPlayerEvent::Paused:
            s_paused = true;
            UpdateControls(true, false);
            SetStatusDirect(I18n::T("已暂停"));
            break;
        case PodcastPlayerEvent::Finished:
            s_playing = false;
            s_paused = false;
            UpdateControls(false, false);
            SetStatusDirect(I18n::T("播放结束"));
            break;
        case PodcastPlayerEvent::Error:
            s_playing = false;
            s_paused = false;
            UpdateControls(false, false);
            SetStatusDirect(event->detail.empty()
                                ? I18n::T("播放失败")
                                : event->detail.c_str());
            break;
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
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    PodcastPlayer::Stop();
    s_playing = false;
    s_paused = false;
    UpdateControls(false, false);
    SetStatusDirect(I18n::T("已停止"));
}

void ReturnHome() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_screen_active.store(false, std::memory_order_release);
    PodcastPlayer::Stop();

    lv_obj_t* old_screen = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_screen != nullptr && old_screen != home) {
        lv_obj_delete_async(old_screen);
    }
}

void OnBackClicked(lv_event_t* /*event*/) {
    ReturnHome();
}

void OnSwipeBack() {
    ReturnHome();
}

void OnScreenUnloaded(lv_event_t* event) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (target != s_bound_screen) {
        return;
    }
    s_bound_screen = nullptr;
    s_screen_active.store(false, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    PodcastPlayer::SetEventCallback(nullptr, nullptr);
    PodcastPlayer::Stop();
    s_current_title.clear();
    s_playing = false;
    s_paused = false;
    s_ui = PodcastUi{};
}

void BuildHeader(lv_obj_t* screen) {
    lv_obj_t* back = lv_button_create(screen);
    lv_obj_set_size(back, 58, 58);
    lv_obj_set_pos(back, 22, 14);
    lv_obj_set_style_radius(back, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1B2740), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(back, true);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_font(back_label, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(back_label);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, I18n::T("一丢播客"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    screen_make_input_passive(title);
}

}  // namespace

lv_obj_t* PodcastScreen::Create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.screen = screen;

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
    lv_obj_set_width(s_ui.status, 640);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 560);
    screen_make_input_passive(s_ui.status);

    s_ui.pause_label = nullptr;
    s_ui.pause_btn =
        CreateActionButton(screen, 180, I18n::T("暂停"), OnPauseClicked);
    s_ui.pause_label =
        lv_obj_get_child(s_ui.pause_btn, 0);
    s_ui.stop_btn =
        CreateActionButton(screen, 390, I18n::T("停止"), OnStopClicked);
    UpdateControls(false, false);

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
        PodcastPlayer::SetEventCallback(OnPlayerEvent, nullptr);
        UpdateControls(false, false);
        if (!s_items.empty()) {
            RebuildList();
            SetStatusDirect(I18n::T("选择节目开始收听"));
        }
        StartLoadWorker();
    } else {
        ESP_LOGI(TAG, "unload: podcast_screen");
        s_screen_active.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        PodcastPlayer::Stop();
    }
}
