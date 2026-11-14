#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#include <ssid_manager.h>
#include <inttypes.h>

void DiagLogEnableUdp(bool enable);

#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif

// 回声诊断：播放 TTS 时运行通信 AFE 并打印 mic/ref/aec_out 能量，
// 但不上传音频、不触发 VAD 打断，用于确认参考通道是否有效。
// 自供 AEC 参考 + 双讲门限已验证可用，这里切换为生产行为：
// 播放 TTS 时仍运行通信 AFE，但正常上行语音，并据双讲门限触发打断。
constexpr bool kEchoDiagnostics = false;
constexpr bool kBargeInByVad = true;
// 自供 AEC 参考延迟：实测 40~100ms 效果最好，固定使用，不再轮换
constexpr int kSelfAecDelayMs = 40;

#ifdef HAVE_LVGL
#include "ota_screen.h"
#include "home_screen.h"
#include "chat_screen/chat_screen.h"
#endif

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

    // 回声保护(AEC)默认关；开机后由 ApplyInterruptPreferenceFromNvs() 按 NVS 恢复
    aec_mode_ = kAecOff;

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    CancelVoiceUiHardRelease();
    if (voice_ui_start_retry_timer_ != nullptr) {
        esp_timer_stop(voice_ui_start_retry_timer_);
        esp_timer_delete(voice_ui_start_retry_timer_);
        voice_ui_start_retry_timer_ = nullptr;
    }
    if (voice_ui_release_timer_ != nullptr) {
        esp_timer_delete(voice_ui_release_timer_);
        voice_ui_release_timer_ = nullptr;
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();


    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota.GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            // Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        // ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        while (activation_suspended_) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            while (activation_suspended_) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                pending_activation_code_.clear();
#ifdef HAVE_LVGL
                HomeScreen::RefreshStatusBar();
#endif
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    if (activation_suspended_) {
        return;
    }

    // OTA 激活：仅缓存验证码供状态栏展示，不 Alert、不播报数字音。
    pending_activation_code_ = code;
#ifdef HAVE_LVGL
    HomeScreen::RefreshStatusBar();
#endif
    ESP_LOGI(TAG, "Activation code ready for status bar (no TTS): %s (%s)",
             code.c_str(), message.c_str());
}

void Application::SetActivationSuspended(bool suspended) {
    activation_suspended_ = suspended;
    if (suspended) {
        DismissAlert();
        ESP_LOGI(TAG, "Activation suspended for stress test");
    } else {
        ESP_LOGI(TAG, "Activation resumed after stress test");
    }
}

bool Application::IsDeviceActivated() const {
    // starting / 联网 / OTA 中：尚未标记 boot_ready_
    if (!boot_ready_) {
        return false;
    }
    // 仍有激活码待绑定
    if (HasPendingActivation()) {
        return false;
    }
    // 激活流程进行中（含中途被切到其它态前的 activating）
    if (device_state_ == kDeviceStateActivating) {
        return false;
    }
    return true;
}

void Application::StopSystemAudioForStressTest() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }

    if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (device_state_ == kDeviceStateListening && protocol_) {
        protocol_->SendStopListening();
    }

    audio_service_.EnableAudioTesting(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ResetDecoder();

    for (int i = 0; i < 20 && !audio_service_.IsIdle(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking ||
        device_state_ == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
    }

    DismissAlert();
    ESP_LOGI(TAG, "System audio stopped for stress test");
}

void Application::RestoreSystemAudioAfterStressTest() {
    // 唤醒词只属于语音 UI 会话；压力测试结束后由聊天/数字人页重新 SetVoiceUiDesired(true)。
    if (voice_ui_active_ && device_state_ == kDeviceStateIdle) {
        audio_service_.EnableWakeWordDetection(true);
    }
    ESP_LOGI(TAG, "System audio restored after stress test (voice_ui=%d)",
             voice_ui_active_ ? 1 : 0);
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty() && !activation_suspended_) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        // 激活中点按：回到 Idle，但不自动开唤醒词（需在语音 UI 会话内）。
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!voice_ui_active_) {
        ESP_LOGW(TAG, "ToggleChatState ignored: voice UI session inactive");
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!voice_ui_active_ || !protocol_) {
                return;
            }
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeAutoStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!voice_ui_active_) {
        ESP_LOGW(TAG, "StartListening ignored: voice UI session inactive");
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!voice_ui_active_ || !protocol_) {
                return;
            }
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // OTA URL：NVS(wifi/ota_url) 为空则写入默认地址，非空则沿用 NVS
    {
        static constexpr const char* kDefaultOtaUrl = "https://api.tenclass.net/xiaozhi/ota/";
        Settings settings("wifi", true);
        std::string ota_url = settings.GetString("ota_url");
        if (ota_url.empty()) {
            ota_url = kDefaultOtaUrl;
            settings.SetString("ota_url", ota_url);
            ESP_LOGI(TAG, "OTA URL empty in NVS, wrote default: %s", ota_url.c_str());
        } else {
            ESP_LOGI(TAG, "OTA URL from NVS: %s", ota_url.c_str());
        }
    }

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    //直接校验OTA
    Ota ota;
    ota.MarkCurrentVersionValid();

// #if CONFIG_ESP_HOSTED_ENABLED
//     /* Boot-time probe: C5 ESP-Hosted slave (WiFi coprocessor) present? */
//     ESP_LOGI(TAG, "C5 hosted check: connecting to slave...");
//     if (esp_hosted_connect_to_slave() == ESP_OK) {
//         esp_hosted_coprocessor_fwver_t fwver{};
//         uint32_t chip_id = 0;
//         char target_name[32] = {0};
//         if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
//             ESP_LOGI(TAG,
//                      "C5 hosted check: OK — FW %" PRIu32 ".%" PRIu32 ".%" PRIu32
//                      " (rev=%" PRId32 ")",
//                      fwver.major1, fwver.minor1, fwver.patch1, fwver.revision);
//         } else {
//             ESP_LOGW(TAG, "C5 hosted check: transport up, but fwversion RPC failed");
//         }
//         if (esp_hosted_get_cp_info(&chip_id, target_name, sizeof(target_name)) == ESP_OK) {
//             ESP_LOGI(TAG, "C5 hosted check: chip_id=0x%" PRIx32 " target=%s",
//                      chip_id, target_name[0] ? target_name : "(n/a)");
//         }
//     } else {
//         ESP_LOGE(TAG,
//                  "C5 hosted check: FAIL — slave not reachable "
//                  "(no hosted FW / SDIO / reset?)");
//         PlaySound(Lang::Sounds::OGG_ERR_REG);
//     }
// #endif

    /* Wait for the network to be ready */
    board.StartNetwork();
    // lwIP 就绪后再启用 UDP 诊断通道（提前创建 socket 会导致启动崩溃）
    // kSelfAecDelayMs 为自供 AEC 参考延迟，由 SetDeviceState 在播放时设置
    DiagLogEnableUdp(true);

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

   
    // Check for new assets version
    // CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    // Ota ota;
    CheckNewVersion(ota);
    //加载唤醒词模型
    GetAudioService().SetModelsList(esp_srmodel_init("model"));
    GetAudioService().EnableWakeWordDetection(false);
    // 模型就绪后再同步回声保护(AEC)偏好，避免处理器尚未初始化
    ApplyInterruptPreferenceFromNvs();

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this, display]() {
                    if (esp_timer_get_time() < abort_suppress_until_us_) {
                        // 本地打断后服务端可能还在发旧 TTS，忽略迟到的 start，
                        // 否则会被拉回 Speaking 继续播放，导致用户这句话识别不到。
                        ESP_LOGW(TAG, "ignore stale tts start after abort");
                        return;
                    }
                    display->BeginAssistantRound();
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
#ifdef HAVE_LVGL
                // 后端未接入设备 MCP 时的本地兜底：
                // “打开/听/一丢 + 播客”直接进入播客页，不再等 LLM 调工具。
                std::string user_text(text->valuestring);
                const bool wants_podcast =
                    user_text.find("播客") != std::string::npos &&
                    (user_text.find("打开") != std::string::npos ||
                     user_text.find("一丢") != std::string::npos ||
                     user_text.find("听") != std::string::npos);
                if (wants_podcast) {
                    Schedule([this]() {
                        ESP_LOGI(TAG, "local intent: open podcast");
                        if (device_state_ == kDeviceStateSpeaking) {
                            AbortSpeaking(kAbortReasonNone);
                        } else if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->SendStopListening();
                        }
                        HomeScreen::RequestOpenPodcastFromCurrentScreen();
                    });
                }
#endif
            }
        } else if (strcmp(type->valuestring, "abort") == 0) {
            // 服务端主动下发中断：停播放并切回聆听（对应方案中的 server→device abort）。
            ESP_LOGI(TAG, "server abort received");
            Schedule([this]() {
                if (device_state_ == kDeviceStateSpeaking) {
                    aborted_ = true;
                    SetListeningMode(kListeningModeAutoStop);
                }
            });
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    // 启动流水线完成（联网/OTA/激活/协议）后才标记就绪；勿把 starting 或中途 Idle 当已激活
    boot_ready_ = true;
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        // audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            if (voice_ui_active_) {
                OnWakeWordDetected();
            }
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            } else if (kBargeInByVad &&
                       device_state_ == kDeviceStateSpeaking &&
                       aec_mode_ == kAecOnDeviceSide &&
                       audio_service_.IsAudioProcessorRunning() &&
                       esp_timer_get_time() >= barge_in_ready_at_us_ &&
                       audio_service_.IsVoiceDetected()) {
                // 本地 VAD 只会在 AEC 之后检测到外部语音时触发；
                // 立即停止本轮 TTS 并转入聆听，不再依赖服务端判断。
                ESP_LOGI(TAG, "barge-in detected, abort speaking");
                AbortSpeaking(kAbortReasonNone);
                SetListeningMode(kListeningModeAutoStop);
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!voice_ui_active_ || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                if (voice_ui_active_) {
                    audio_service_.EnableWakeWordDetection(true);
                }
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(kListeningModeAutoStop);
#else
        SetListeningMode(kListeningModeAutoStop);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        // 打断开启时，播放中听到唤醒词立即停止本轮回答并进入聆听。
        if (aec_mode_ != kAecOff) {
            ESP_LOGI(TAG, "wake word during speaking, interrupt");
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeAutoStop);
        }
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    abort_suppress_until_us_ = esp_timer_get_time() + 2500000;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.SetBargeInActive(false);
            audio_service_.SetVoiceUploadMuted(false);
            audio_service_.SetSelfAecReference(false);
            audio_service_.EnableVoiceProcessing(false);
            // 唤醒词仅在聊天/数字人会话内开启；桌面必须 Release 掉 AFE。
            if (voice_ui_active_) {
                audio_service_.EnableWakeWordDetection(true);
            } else {
                audio_service_.EnableWakeWordDetection(false);
                audio_service_.ReleaseWakeWordEngine();
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening: {
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            // 打断后进入聆听：继续保留通信 AFE 与自供参考，仅关闭打断门限；
            // 恢复正常上行与人声判定，等待用户说话。
            audio_service_.SetVoiceUploadMuted(false);
            audio_service_.SetSelfAecReference(false);
            audio_service_.SetBargeInActive(false);
            if (aec_mode_ == kAecOnDeviceSide) {
                audio_service_.GetAudioProcessor()->SetEchoGate(false);
            }

            // 从 Speaking（本地打断）进入 Listening 时 AFE 已在运行，
            // 仍需补发 listen start，否则服务端不会开始接收本轮语音。
            bool need_start_listening = (previous_state == kDeviceStateSpeaking);
            if (previous_state == kDeviceStateSpeaking ||
                !audio_service_.IsAudioProcessorRunning()) {
                // 打断后必须清空未播放完的 TTS 并重置 AFE：
                // 否则设备会继续放自己的尾音，服务端只能听到回声，识别不到用户。
                need_start_listening = true;
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            if (need_start_listening && protocol_) {
                protocol_->SendStartListening(listening_mode_);
            }
            break;
        }
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            // 该模组的 AEC 参考通道无法消掉外放回声，通信 AFE 的 VAD 会把
            // 自己的声音当成用户语音，导致每句话都被自己打断。
            // 因此播放 TTS 时只保留唤醒词引擎（带 AEC）：
            // 用户说“你好一丢”即可打断，且不会被任意回声误触发。
            if (aec_mode_ != kAecOff) {
                // 本机 AEC 参考通道无效（ref 恒为 0），全双工上行必然把扬声器
                // 回声送给服务端并触发误打断。因此播放 TTS 时不上行麦克风，
                // 但唤醒词 AFE 同样依赖 R 通道做 AEC；把实际播放 PCM 注入参考
                // 通道，外放时说“你好一丢”才能被检测到并打断本轮回答。
                audio_service_.SetVoiceUploadMuted(true);
                audio_service_.SetSelfAecReference(true);
                audio_service_.SetSelfAecDelayMs(kSelfAecDelayMs);
                audio_service_.GetAudioProcessor()->SetEchoGate(false);
                audio_service_.SetBargeInActive(false);
                barge_in_ready_at_us_ = esp_timer_get_time() + 1200000;
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(true);
            } else {
                audio_service_.SetVoiceUploadMuted(false);
                audio_service_.SetSelfAecReference(false);
                audio_service_.GetAudioProcessor()->SetEchoGate(false);
                audio_service_.SetBargeInActive(false);
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        default:
            // Do nothing
            break;
    }

#ifdef HAVE_LVGL
    ChatScreen::RefreshDeviceState();
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // 重启前关背光，避免过渡花屏/蓝屏；不写 NVS，下次启动仍按原亮度恢复。
    if (Backlight* bl = Board::GetInstance().GetBacklight()) {
        bl->SetBrightness(0, false);
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    // 等待背光渐暗（SetBrightness 约 5ms/级）后再重启
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    SetDeviceState(kDeviceStateUpgrading);

#ifdef HAVE_LVGL
    std::string version_line = std::string(Lang::Strings::NEW_VERSION) + version_info;
    OtaScreen::Show(version_line.c_str());
#else
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());
#endif

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [](int progress, size_t downloaded, size_t total, size_t speed) {
#ifdef HAVE_LVGL
        OtaScreen::Update(progress, downloaded, total, speed);
#else
        (void)progress;
        (void)downloaded;
        (void)total;
        (void)speed;
#endif
    });

    if (!upgrade_success) {
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
#ifdef HAVE_LVGL
        OtaScreen::Dismiss();
#endif
        audio_service_.Start();
        board.SetPowerSaveMode(true);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
#ifdef HAVE_LVGL
    OtaScreen::SetStatusMessage("升级成功，即将重启...");
#else
    display->SetChatMessage("system", "Upgrade successful, rebooting...");
#endif
    vTaskDelay(pdMS_TO_TICKS(1000));
    Reboot();
    return true;
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!voice_ui_active_ || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                if (voice_ui_active_) {
                    audio_service_.EnableWakeWordDetection(true);
                }
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(kListeningModeAutoStop);
#else
        SetListeningMode(kListeningModeAutoStop);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    if (mode != kAecOff && mode != kAecOnDeviceSide && mode != kAecOnServerSide) {
        ESP_LOGW(TAG, "unsupported AecMode %d, fallback Off", static_cast<int>(mode));
        mode = kAecOff;
    }

    aec_mode_ = mode;
    {
        // NVS 键最长 15 字符；interrupt 记忆「是否开启回声保护」
        Settings settings("audio", true);
        settings.SetBool("interrupt", aec_mode_ != kAecOff);
    }
    // 只更新偏好，不在播放/聆听中重建 AFE；下一轮 EnableVoiceProcessing
    // 会读取该偏好并应用。
    if (!audio_service_.IsAudioProcessorRunning()) {
        audio_service_.EnableDeviceAec(aec_mode_ == kAecOnDeviceSide);
    }

    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        // 点击瞬间不创建/销毁 AFE：空闲态两套 AFE 同时常驻会硬复位。
        // 偏好由下一次 EnableVoiceProcessing 时同步到通信 AFE。
        if (aec_mode_ != kAecOff) {
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
        } else {
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
        }

        // 切换偏好后关闭长连接，下一轮重新建立聆听会话。
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::ApplyInterruptPreferenceFromNvs() {
    Settings settings("audio");
    // 播放期间的“打断”依赖唤醒词 AFE：把实际播放 PCM 注入 R 参考通道，
    // 说“你好一丢”即可停 TTS 并进入聆听；不做麦克风持续上行。
    const bool interrupt_on = settings.GetBool("interrupt", true);
    aec_mode_ = interrupt_on ? kAecOnDeviceSide : kAecOff;
    audio_service_.EnableDeviceAec(aec_mode_ == kAecOnDeviceSide);
    ESP_LOGI(TAG, "interrupt preference: %s (wake-word AEC)", interrupt_on ? "on" : "off");
}

void Application::PlaySound(const std::string_view& sound) {
    if (activation_suspended_) {
        return;
    }
    audio_service_.PlaySound(sound);
}

void Application::SoftStopVoiceAudioPaths() {
    audio_service_.SetBargeInActive(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.EnableVoiceProcessing(false);
}

void Application::TearDownVoiceAudioPaths(bool release_wake_word) {
    SoftStopVoiceAudioPaths();
    if (release_wake_word) {
        audio_service_.ReleaseWakeWordEngine();
    }
}

void Application::CancelVoiceUiHardRelease() {
    if (voice_ui_release_timer_ != nullptr) {
        esp_timer_stop(voice_ui_release_timer_);
    }
}

void Application::ScheduleVoiceUiHardRelease(uint32_t epoch) {
    CancelVoiceUiHardRelease();
    voice_ui_pending_release_epoch_ = epoch;

    if (voice_ui_release_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    const uint32_t epoch = app->voice_ui_pending_release_epoch_;
                    app->Schedule([app, epoch]() {
                        if (app->voice_ui_desired_ || app->voice_ui_epoch_ != epoch) {
                            return;
                        }
                        ESP_LOGI(TAG, "voice UI delayed hard release (epoch=%" PRIu32 ")", epoch);
                        app->ApplyVoiceUiStop();
                    });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "voice_ui_rel",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &voice_ui_release_timer_);
    }

    // 停 Feed 后短暂保留 AFE：快速再进语音页可复用，并让旧屏内存先释放。
    constexpr uint64_t kReleaseDelayUs = 1200 * 1000;
    esp_timer_start_once(voice_ui_release_timer_, kReleaseDelayUs);
}

void Application::ScheduleVoiceUiStartRetry(uint32_t epoch) {
    voice_ui_pending_retry_epoch_ = epoch;
    if (voice_ui_start_retry_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    const uint32_t epoch = app->voice_ui_pending_retry_epoch_;
                    app->Schedule([app, epoch]() {
                        if (!app->voice_ui_desired_ || app->voice_ui_epoch_ != epoch) {
                            return;
                        }
                        app->ApplyVoiceUiStart();
                    });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "voice_ui_retry",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &voice_ui_start_retry_timer_);
    } else {
        esp_timer_stop(voice_ui_start_retry_timer_);
    }
    esp_timer_start_once(voice_ui_start_retry_timer_, 200 * 1000);
}

bool Application::TryEnableWakeWordForVoiceUi() {
    if (audio_service_.IsWakeWordEngineReady()) {
        audio_service_.EnableWakeWordDetection(true);
        return audio_service_.IsWakeWordEngineReady();
    }

    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    // WakeNet AFE 创建失败时可能返回非空坏句柄并在卷积中 Load fault；堆过低则推迟。
    constexpr size_t kMinInternal = 48 * 1024;
    constexpr size_t kMinPsram = 256 * 1024;
    if (free_internal < kMinInternal || free_psram < kMinPsram) {
        ESP_LOGW(TAG,
                 "Defer wake word init (internal=%uKB psram=%uKB)",
                 (unsigned)(free_internal / 1024), (unsigned)(free_psram / 1024));
        return false;
    }

    audio_service_.EnableWakeWordDetection(true);
    if (!audio_service_.IsWakeWordEngineReady()) {
        ESP_LOGW(TAG, "Wake word init failed, will retry");
        return false;
    }
    return true;
}

void Application::SetVoiceUiDesired(bool desired) {
    if (voice_ui_desired_ == desired) {
        ESP_LOGD(TAG, "voice UI desired unchanged (%d)", desired ? 1 : 0);
        return;
    }
    voice_ui_desired_ = desired;
    ++voice_ui_epoch_;
    const uint32_t epoch = voice_ui_epoch_;
    ESP_LOGI(TAG, "voice UI desired -> %d (epoch=%" PRIu32 ")", desired ? 1 : 0, epoch);

    if (!desired) {
        // 立刻软停（降 CPU），硬 destroy 延后；若很快再进页则取消 destroy。
        voice_ui_active_ = false;
        SoftStopVoiceAudioPaths();
        if (voice_ui_start_retry_timer_ != nullptr) {
            esp_timer_stop(voice_ui_start_retry_timer_);
        }
        ScheduleVoiceUiHardRelease(epoch);
        Schedule([this]() { SyncVoiceUiSession(); });
        return;
    }

    CancelVoiceUiHardRelease();
    Schedule([this]() { SyncVoiceUiSession(); });
}

void Application::ParkVoiceUiProtocol() {
    if (device_state_ == kDeviceStateListening && protocol_) {
        protocol_->CloseAudioChannel();
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    } else if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }

    if (device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking ||
        device_state_ == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::SyncVoiceUiSession() {
    if (voice_ui_desired_) {
        ApplyVoiceUiStart();
        // Start 中途又 leave：禁止留下唤醒词
        if (!voice_ui_desired_) {
            SoftStopVoiceAudioPaths();
            voice_ui_active_ = false;
            ScheduleVoiceUiHardRelease(voice_ui_epoch_);
        }
    } else {
        // 硬释放交给延迟定时器；这里只停协议，避免抵消 debounce
        voice_ui_active_ = false;
        SoftStopVoiceAudioPaths();
        ParkVoiceUiProtocol();
    }
}

void Application::ApplyVoiceUiStart() {
    if (!voice_ui_desired_) {
        SoftStopVoiceAudioPaths();
        voice_ui_active_ = false;
        ESP_LOGI(TAG, "ApplyVoiceUiStart skipped (desired=0)");
        return;
    }

    const uint32_t epoch = voice_ui_epoch_;

    if (voice_ui_active_) {
        if (device_state_ == kDeviceStateIdle) {
            audio_service_.EnableVoiceProcessing(false);
            if (!TryEnableWakeWordForVoiceUi()) {
                ScheduleVoiceUiStartRetry(epoch);
                return;
            }
        }
        ESP_LOGI(TAG, "voice UI already active");
        return;
    }

    ESP_LOGI(TAG, "ApplyVoiceUiStart");
    voice_ui_active_ = true;

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EnableVoiceProcessing(false);
        if (!TryEnableWakeWordForVoiceUi()) {
            voice_ui_active_ = false;
            ScheduleVoiceUiStartRetry(epoch);
            return;
        }
        if (!voice_ui_desired_) {
            SoftStopVoiceAudioPaths();
            voice_ui_active_ = false;
            return;
        }
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
    } else if (device_state_ != kDeviceStateActivating &&
               device_state_ != kDeviceStateStarting &&
               device_state_ != kDeviceStateWifiConfiguring &&
               device_state_ != kDeviceStateAudioTesting &&
               device_state_ != kDeviceStateUpgrading) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ApplyVoiceUiStop() {
    voice_ui_active_ = false;
    TearDownVoiceAudioPaths(true);
    ParkVoiceUiProtocol();
    SoftStopVoiceAudioPaths();
    ESP_LOGI(TAG, "ApplyVoiceUiStop: AFE destroyed, protocol parked");
}
