#include "english_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "application.h"
#include "chat_screen/chat_screen.h"
#include "english_client.h"
#include "english_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "home_screen/home_screen.h"
#include "i18n.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "EnglishScreen";
constexpr int32_t kPanelSize = 720;
constexpr int32_t kContentX = 24;
constexpr int32_t kContentY = 100;
constexpr int32_t kContentWidth = 672;
constexpr int32_t kContentHeight = 480;
constexpr uint32_t kColorBg = 0x0B1020;
constexpr uint32_t kColorPanel = 0x111827;
constexpr uint32_t kColorCard = 0x182033;
constexpr uint32_t kColorCardPressed = 0x24314D;
constexpr uint32_t kColorAccent = 0x60A5FA;
constexpr uint32_t kColorSuccess = 0x34D399;
constexpr uint32_t kColorWarning = 0xFBBF24;
constexpr uint32_t kColorText = 0xF8FAFC;
constexpr uint32_t kColorSubtle = 0x94A3B8;

enum class RequestKind : uint8_t {
    Tasks,
    Review,
    Result,
};

struct EnglishUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* progress = nullptr;
    lv_obj_t* stage_caption = nullptr;
    lv_obj_t* card = nullptr;
    lv_obj_t* card_label = nullptr;
};

struct RequestResult {
    uint32_t session = 0;
    uint32_t generation = 0;
    RequestKind kind = RequestKind::Tasks;
    EnglishTaskList tasks;
    EnglishReview review;
    std::string task_id;
    std::string word_id;
    std::string result;
    std::string error;
    int index = 0;
};

struct ResumeChatContext {
    uint32_t session = 0;
};

EnglishUi s_ui;
lv_obj_t* s_bound_screen = nullptr;
lv_obj_t* s_return_screen = nullptr;
EnglishTaskList s_tasks;
EnglishReview s_review;
std::string s_current_task_id;
size_t s_current_index = 0;
uint8_t s_stage = 0;
bool s_in_review = false;
bool s_tasks_cached = false;
time_t s_tasks_cached_at = 0;
std::string s_review_cached_task_id;
EnglishReview s_review_cached;
time_t s_review_cached_at = 0;
bool s_restore_chat_on_unload = false;
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_busy{false};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_request_generation{0};

uint32_t CurrentSession() {
    return s_session.load(std::memory_order_relaxed);
}

bool IsCurrentRequest(const RequestResult& result) {
    return s_screen_active.load(std::memory_order_relaxed) &&
           result.session == CurrentSession() &&
           result.generation ==
               s_request_generation.load(std::memory_order_relaxed);
}

void SetStatus(const char* text) {
    if (s_ui.status != nullptr) {
        lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    }
}

bool TasksCacheFresh() {
    return s_tasks_cached && !s_tasks.empty() &&
           std::time(nullptr) - s_tasks_cached_at < 60;
}

bool ReviewCacheFresh(const std::string& task_id) {
    return !s_review_cached_task_id.empty() &&
           s_review_cached_task_id == task_id &&
           !s_review_cached.entries.empty() &&
           std::time(nullptr) - s_review_cached_at < 600;
}

void ResetContentRefs() {
    s_ui.progress = nullptr;
    s_ui.stage_caption = nullptr;
    s_ui.card = nullptr;
    s_ui.card_label = nullptr;
}

lv_obj_t* CreateButton(lv_obj_t* parent, int32_t x, int32_t y,
                       int32_t width, int32_t height, const char* text,
                       uint32_t color, lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(button, true);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0B1020),
                                LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void ConfigurePageContent() {
    lv_obj_set_style_bg_color(s_ui.content,
                              lv_color_hex(kColorPanel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.content, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.content, 0, LV_PART_MAIN);
    lv_obj_set_style_layout(s_ui.content, LV_LAYOUT_NONE, 0);
    lv_obj_set_scrollbar_mode(s_ui.content, LV_SCROLLBAR_MODE_OFF);
}

void ConfigureListContent() {
    ConfigurePageContent();
    lv_obj_set_style_pad_all(s_ui.content, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.content, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_ui.content, LV_SCROLLBAR_MODE_AUTO);
}

void ShowCenteredMessage(const char* text, bool show_back_button);
void OnBackToListClicked(lv_event_t* /*event*/);
void OnTaskClicked(lv_event_t* event);
void OnCardClicked(lv_event_t* /*event*/);
void OnRememberedClicked(lv_event_t* /*event*/);
void OnForgottenClicked(lv_event_t* /*event*/);
void StartTasksWorker();
void StartReviewWorker(const std::string& task_id);
void StartSubmitWorker(const std::string& result);
void BuildTaskList();
void BuildReview();
void ResumeChatAfterVoiceUiDrain(uint32_t session);

void ShowCenteredMessage(const char* text, bool show_back_button) {
    ResetContentRefs();
    lv_obj_clean(s_ui.content);
    ConfigurePageContent();

    lv_obj_t* label = lv_label_create(s_ui.content);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, kContentWidth - 64);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0,
                 show_back_button ? -40 : 0);
    screen_make_input_passive(label);

    if (show_back_button) {
        CreateButton(s_ui.content, 206, 330, 260, 64,
                     I18n::T("返回任务"), kColorAccent,
                     OnBackToListClicked);
    }
}

void BuildTaskList() {
    s_in_review = false;
    ResetContentRefs();
    lv_obj_clean(s_ui.content);
    ConfigureListContent();

    if (s_tasks.empty()) {
        lv_obj_t* hint = lv_label_create(s_ui.content);
        lv_label_set_text(hint, I18n::T("还未收到任务"));
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_width(hint, kContentWidth - 32);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        screen_make_input_passive(hint);
        return;
    }

    for (size_t index = 0; index < s_tasks.size(); ++index) {
        const EnglishTask& task = s_tasks[index];
        lv_obj_t* row = lv_obj_create(s_ui.content);
        lv_obj_set_size(row, kContentWidth - 8, 92);
        lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCardPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            row, OnTaskClicked, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<uintptr_t>(index)));

        char meta_buffer[96];
        std::snprintf(meta_buffer, sizeof(meta_buffer), "%s · %d 个短句",
                      task.task_date.c_str(), task.phrase_count);
        lv_obj_t* meta = lv_label_create(row);
        lv_label_set_text(meta, meta_buffer);
        lv_obj_set_style_text_font(meta, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(meta, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_align(meta, LV_ALIGN_TOP_LEFT, 0, 0);
        screen_make_input_passive(meta);

        lv_obj_t* state = lv_label_create(row);
        const char* state_text = task.status == "completed"
                                     ? I18n::T("已完成")
                                     : task.status == "in_progress"
                                           ? I18n::T("进行中")
                                           : I18n::T("未开始");
        lv_label_set_text(state, state_text);
        lv_obj_set_style_text_font(state, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            state, lv_color_hex(task.status == "completed"
                                    ? kColorSuccess
                                    : kColorSubtle),
            LV_PART_MAIN);
        lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        screen_make_input_passive(state);

        lv_obj_t* action = lv_label_create(row);
        lv_label_set_text(action, I18n::T("复习 >"));
        lv_obj_set_style_text_font(action, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(action, lv_color_hex(kColorAccent),
                                    LV_PART_MAIN);
        lv_obj_align(action, LV_ALIGN_RIGHT_MID, 0, 0);
        screen_make_input_passive(action);
    }
}

void UpdateCard() {
    if (s_ui.card_label == nullptr || s_ui.stage_caption == nullptr ||
        s_ui.progress == nullptr || s_current_index >= s_review.entries.size()) {
        return;
    }
    const EnglishEntry& entry = s_review.entries[s_current_index];
    const char* caption = I18n::T("中文含义");
    const char* text = entry.meaning.c_str();
    if (s_stage == 1) {
        caption = I18n::T("英文短句");
        text = entry.phrase.c_str();
    } else if (s_stage == 2) {
        caption = I18n::T("场景例句");
        text = entry.example.c_str();
    }
    lv_label_set_text(s_ui.stage_caption, caption);
    lv_label_set_text(s_ui.card_label, text);

    char progress[32];
    std::snprintf(progress, sizeof(progress), "%u / %u",
                  static_cast<unsigned>(s_current_index + 1),
                  static_cast<unsigned>(s_review.entries.size()));
    lv_label_set_text(s_ui.progress, progress);
}

void BuildReview() {
    s_in_review = true;
    ResetContentRefs();
    lv_obj_clean(s_ui.content);
    ConfigurePageContent();
    if (s_review.entries.empty()) {
        ShowCenteredMessage(I18n::T("暂无复习短句"), true);
        return;
    }

    s_ui.stage_caption = lv_label_create(s_ui.content);
    lv_obj_set_style_text_font(s_ui.stage_caption, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.stage_caption,
                                lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_align(s_ui.stage_caption, LV_ALIGN_TOP_MID, 0, 8);
    screen_make_input_passive(s_ui.stage_caption);

    s_ui.progress = lv_label_create(s_ui.content);
    lv_obj_set_style_text_font(s_ui.progress, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.progress,
                                lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_align(s_ui.progress, LV_ALIGN_TOP_RIGHT, -16, 8);
    screen_make_input_passive(s_ui.progress);

    s_ui.card = lv_obj_create(s_ui.content);
    lv_obj_set_pos(s_ui.card, 16, 54);
    lv_obj_set_size(s_ui.card, kContentWidth - 32, 300);
    lv_obj_set_style_radius(s_ui.card, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.card, lv_color_hex(kColorCard),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.card, lv_color_hex(kColorCardPressed),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_ui.card, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.card, OnCardClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_ui.card, true);

    s_ui.card_label = lv_label_create(s_ui.card);
    lv_obj_set_width(s_ui.card_label, kContentWidth - 96);
    lv_label_set_long_mode(s_ui.card_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.card_label, &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.card_label,
                                lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.card_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_center(s_ui.card_label);
    screen_make_input_passive(s_ui.card_label);

    lv_obj_t* hint = lv_label_create(s_ui.content);
    lv_label_set_text(hint, I18n::T("轻触卡片翻面"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 366);
    screen_make_input_passive(hint);

    CreateButton(s_ui.content, 40, 402, 280, 64,
                 I18n::T("还需努力"), kColorWarning,
                 OnForgottenClicked);
    CreateButton(s_ui.content, 352, 402, 280, 64,
                 I18n::T("已经掌握"), kColorSuccess,
                 OnRememberedClicked);
    UpdateCard();
}

void BuildComplete() {
    s_in_review = false;
    ShowCenteredMessage(
        I18n::T("本次复习完成\n未掌握的短句已加入成长记录"), true);
    SetStatus(I18n::T("复习完成"));
}

void PostRequestResult(RequestResult* result) {
    if (lv_async_call(
            [](void* data) {
                std::unique_ptr<RequestResult> resolved(
                    static_cast<RequestResult*>(data));
                if (!IsCurrentRequest(*resolved)) {
                    return;
                }
                s_busy.store(false, std::memory_order_release);
                if (!resolved->error.empty()) {
                    SetStatus(resolved->error.c_str());
                    return;
                }
                switch (resolved->kind) {
                    case RequestKind::Tasks:
                        s_tasks = std::move(resolved->tasks);
                        s_tasks_cached = true;
                        s_tasks_cached_at = std::time(nullptr);
                        BuildTaskList();
                        SetStatus(s_tasks.empty()
                                      ? I18n::T("还未收到任务")
                                      : I18n::T("选择任务开始复习"));
                        break;
                    case RequestKind::Review:
                        s_review = std::move(resolved->review);
                        s_current_task_id = resolved->task_id;
                        s_current_index = 0;
                        s_stage = 0;
                        s_review_cached_task_id = resolved->task_id;
                        s_review_cached = s_review;
                        s_review_cached_at = std::time(nullptr);
                        BuildReview();
                        if (s_review.entries.empty()) {
                            SetStatus(I18n::T("暂无复习短句"));
                        } else {
                            SetStatus(s_review.label.empty()
                                          ? I18n::T("轻触卡片翻面")
                                          : s_review.label.c_str());
                        }
                        break;
                    case RequestKind::Result:
                        if (resolved->index + 1 <
                            static_cast<int>(s_review.entries.size())) {
                            s_current_index = resolved->index + 1;
                            s_stage = 0;
                            BuildReview();
                            SetStatus(I18n::T("结果已保存"));
                        } else {
                            BuildComplete();
                        }
                        break;
                }
            },
            result) != LV_RESULT_OK) {
        s_busy.store(false, std::memory_order_release);
        delete result;
    }
}

bool BeginRequest(RequestResult*& context) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        delete context;
        return false;
    }
    context->session = CurrentSession();
    context->generation =
        s_request_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    return true;
}

void StartTasksWorker() {
    auto* context = new RequestResult{};
    context->kind = RequestKind::Tasks;
    if (!BeginRequest(context)) {
        return;
    }
    if (xTaskCreate(
            [](void* arg) {
                auto* result = static_cast<RequestResult*>(arg);
                result->tasks = EnglishClient::FetchTasks(result->error);
                PostRequestResult(result);
                vTaskDelete(nullptr);
            },
            "english_tasks", 8192, context, 4, nullptr) != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        delete context;
        SetStatus(I18n::T("加载任务失败"));
    }
}

void StartReviewWorker(const std::string& task_id) {
    auto* context = new RequestResult{};
    context->kind = RequestKind::Review;
    context->task_id = task_id;
    if (!BeginRequest(context)) {
        return;
    }
    if (xTaskCreate(
            [](void* arg) {
                auto* result = static_cast<RequestResult*>(arg);
                result->review =
                    EnglishClient::FetchReview(result->task_id, result->error);
                PostRequestResult(result);
                vTaskDelete(nullptr);
            },
            "english_review", 8192, context, 4, nullptr) != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        delete context;
        SetStatus(I18n::T("加载复习内容失败"));
    }
}

void StartSubmitWorker(const std::string& result) {
    if (s_current_task_id.empty() ||
        s_current_index >= s_review.entries.size()) {
        return;
    }
    auto* context = new RequestResult{};
    context->kind = RequestKind::Result;
    context->task_id = s_current_task_id;
    context->word_id = s_review.entries[s_current_index].word_id;
    context->result = result;
    context->index = static_cast<int>(s_current_index);
    if (!BeginRequest(context)) {
        return;
    }
    SetStatus(I18n::T("正在保存结果..."));
    if (xTaskCreate(
            [](void* arg) {
                auto* request = static_cast<RequestResult*>(arg);
                const bool ok = EnglishClient::SubmitResult(
                    request->task_id, request->word_id, request->result,
                    request->error);
                if (!ok && request->error.empty()) {
                    request->error = "保存结果失败";
                }
                PostRequestResult(request);
                vTaskDelete(nullptr);
            },
            "english_result", 8192, context, 4, nullptr) != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        delete context;
        SetStatus(I18n::T("保存结果失败"));
    }
}

void ResumeChatAfterVoiceUiDrain(uint32_t session) {
    auto* context = new ResumeChatContext{};
    context->session = session;
    if (xTaskCreate(
            [](void* arg) {
                std::unique_ptr<ResumeChatContext> resume(
                    static_cast<ResumeChatContext*>(arg));
                vTaskDelay(pdMS_TO_TICKS(80));
                if (resume->session == CurrentSession()) {
                    lv_async_call(
                        [](void* data) {
                            auto* deferred =
                                static_cast<ResumeChatContext*>(data);
                            if (deferred->session == CurrentSession()) {
                                ChatScreen::ResumeAfterTemporaryLeave();
                            }
                            delete deferred;
                        },
                        resume.release());
                }
                vTaskDelete(nullptr);
            },
            "english_resume", 3072, context, 4, nullptr) != pdPASS) {
        delete context;
        ChatScreen::ResumeAfterTemporaryLeave();
    }
}

void OnTaskClicked(lv_event_t* event) {
    const auto index = static_cast<size_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    if (index >= s_tasks.size()) {
        return;
    }
    s_review = EnglishReview{};
    s_current_index = 0;
    s_stage = 0;
    if (ReviewCacheFresh(s_tasks[index].id)) {
        s_review = s_review_cached;
        s_current_task_id = s_tasks[index].id;
        BuildReview();
        SetStatus(s_review.label.empty() ? I18n::T("轻触卡片翻面")
                                          : s_review.label.c_str());
        return;
    }
    ShowCenteredMessage(I18n::T("正在加载复习内容..."), false);
    SetStatus(I18n::T("正在加载复习内容..."));
    StartReviewWorker(s_tasks[index].id);
}

void OnCardClicked(lv_event_t* /*event*/) {
    s_stage = (s_stage + 1) % 3;
    UpdateCard();
}

void OnBackToListClicked(lv_event_t* /*event*/) {
    s_review = EnglishReview{};
    s_current_task_id.clear();
    s_current_index = 0;
    s_stage = 0;
    BuildTaskList();
    SetStatus(s_tasks.empty() ? I18n::T("还未收到任务")
                              : I18n::T("选择任务开始复习"));
}

void OnRememberedClicked(lv_event_t* /*event*/) {
    StartSubmitWorker("remembered");
}

void OnForgottenClicked(lv_event_t* /*event*/) {
    StartSubmitWorker("forgotten");
}

void ReturnHome() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_screen_active.store(false, std::memory_order_release);

    lv_obj_t* old_screen = lv_screen_active();
    lv_obj_t* target = s_return_screen;
    s_return_screen = nullptr;
    if (target == nullptr || target == old_screen) {
        target = HomeScreen::Create();
    }
    s_restore_chat_on_unload = target == ChatScreen::CurrentScreen();
    lv_screen_load(target);
    if (old_screen != nullptr && old_screen != target) {
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
    s_busy.store(false, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_review = EnglishReview{};
    s_current_task_id.clear();
    s_current_index = 0;
    s_stage = 0;
    s_in_review = false;
    s_ui = EnglishUi{};
    if (s_restore_chat_on_unload) {
        s_restore_chat_on_unload = false;
        ResumeChatAfterVoiceUiDrain(CurrentSession());
    }
}

void BuildHeader(lv_obj_t* screen) {
    lv_obj_t* back = lv_button_create(screen);
    lv_obj_set_size(back, 58, 58);
    lv_obj_set_pos(back, 22, 14);
    lv_obj_set_style_radius(back, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1B2740),
                              LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(back, true);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_font(back_label, &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(back_label);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, I18n::T("一丢英语"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    screen_make_input_passive(title);
}

}  // namespace

lv_obj_t* EnglishScreen::Create(lv_obj_t* return_screen) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui = EnglishUi{};
    s_ui.screen = screen;
    s_return_screen = return_screen;
    s_review = EnglishReview{};
    s_current_task_id.clear();
    s_current_index = 0;
    s_stage = 0;
    s_in_review = false;
    s_restore_chat_on_unload = false;

    BuildHeader(screen);

    s_ui.content = lv_obj_create(screen);
    lv_obj_set_pos(s_ui.content, kContentX, kContentY);
    lv_obj_set_size(s_ui.content, kContentWidth, kContentHeight);
    lv_obj_set_style_bg_color(s_ui.content,
                              lv_color_hex(kColorPanel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.content, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.content, 0, LV_PART_MAIN);

    s_ui.status = lv_label_create(screen);
    lv_label_set_text(s_ui.status, I18n::T("正在加载任务..."));
    lv_obj_set_width(s_ui.status, 640);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 606);
    screen_make_input_passive(s_ui.status);

    s_bound_screen = screen;
    lv_obj_add_event_cb(screen, OnScreenUnloaded,
                        LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(screen, OnSwipeBack);
    return screen;
}

void EnglishScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: english_screen");
        Application::GetInstance().SetVoiceUiDesired(false);
        s_screen_active.store(true, std::memory_order_release);
        s_busy.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        if (!TasksCacheFresh()) {
            s_tasks.clear();
        }
        s_review = EnglishReview{};
        s_current_task_id.clear();
        s_current_index = 0;
        s_stage = 0;
        s_in_review = false;
        BuildTaskList();
        if (TasksCacheFresh()) {
            SetStatus(I18n::T("选择任务开始复习"));
        } else {
            SetStatus(I18n::T("正在加载任务..."));
        }
        if (!TasksCacheFresh()) {
            StartTasksWorker();
        }
    } else {
        ESP_LOGI(TAG, "unload: english_screen");
        s_screen_active.store(false, std::memory_order_release);
        s_busy.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    }
}
