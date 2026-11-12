#include "podcast_client.h"

#include <cctype>
#include <ctime>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "board.h"
#include "cJSON.h"
#include "esp_log.h"
#include "http.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "PodcastClient";
constexpr const char* kSettingsNs = "podcast";
constexpr const char* kKeyApiBase = "api_base";
constexpr const char* kKeyDeviceToken = "device_token";
constexpr int kHttpTimeoutMs = 15000;

std::mutex s_http_mutex;

// 播放地址缓存：签名地址 60 分钟有效，这里保守缓存 45 分钟，
// 点开节目时不必再等一次 /play 往返。
constexpr int kPlayCacheSeconds = 45 * 60;

struct PlayCacheEntry {
    std::string id;
    PodcastPlayInfo info;
    time_t expires = 0;
};

std::mutex s_play_cache_mutex;
std::vector<PlayCacheEntry> s_play_cache;

std::string TrimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' ||
            ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

const char* JsonString(const cJSON* object, const char* primary,
                       const char* fallback = nullptr) {
    const cJSON* item = cJSON_GetObjectItem(object, primary);
    if (!cJSON_IsString(item) && fallback != nullptr) {
        item = cJSON_GetObjectItem(object, fallback);
    }
    return cJSON_IsString(item) ? item->valuestring : "";
}

int JsonInt(const cJSON* object, const char* primary,
            const char* fallback = nullptr) {
    const cJSON* item = cJSON_GetObjectItem(object, primary);
    if (!cJSON_IsNumber(item) && fallback != nullptr) {
        item = cJSON_GetObjectItem(object, fallback);
    }
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

bool JsonBool(const cJSON* object, const char* primary,
              const char* fallback = nullptr) {
    const cJSON* item = cJSON_GetObjectItem(object, primary);
    if (item == nullptr && fallback != nullptr) {
        item = cJSON_GetObjectItem(object, fallback);
    }
    return cJSON_IsTrue(item);
}

const cJSON* FindPodcastArray(const cJSON* root) {
    if (cJSON_IsArray(root)) {
        return root;
    }
    if (!cJSON_IsObject(root)) {
        return nullptr;
    }
    const cJSON* podcasts = cJSON_GetObjectItem(root, "podcasts");
    if (cJSON_IsArray(podcasts)) {
        return podcasts;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(data)) {
        return data;
    }
    if (cJSON_IsObject(data)) {
        const cJSON* nested = cJSON_GetObjectItem(data, "podcasts");
        if (cJSON_IsArray(nested)) {
            return nested;
        }
    }
    return nullptr;
}

bool LoadConfig(std::string& api_base, std::string& device_token,
                std::string& error) {
    Settings settings(kSettingsNs, false);
    api_base = TrimTrailingSlash(settings.GetString(kKeyApiBase, CONFIG_PODCAST_API_BASE));
    device_token = settings.GetString(kKeyDeviceToken, CONFIG_PODCAST_DEVICE_TOKEN);
    if (api_base.empty() || device_token.empty()) {
        error = "播客服务未配置\n请联系管理员绑定设备";
        return false;
    }
    return true;
}

bool HttpGet(const std::string& url, const std::string& device_token,
             std::string& body, int& status, std::string& error) {
    std::lock_guard<std::mutex> lock(s_http_mutex);
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "网络未连接";
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        error = "创建网络请求失败";
        return false;
    }

    const std::string authorization = "Bearer " + device_token;
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Connection", "close");
    http->SetHeader("Authorization", authorization);
    if (!http->Open("GET", url)) {
        error = "连接播客服务失败";
        return false;
    }

    status = http->GetStatusCode();
    body = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        if (status == 401 || status == 403) {
            error = "设备未授权";
        } else {
            error = "播客服务返回 HTTP " + std::to_string(status);
        }
        return false;
    }
    return true;
}

}  // namespace

PodcastList PodcastClient::FetchPodcasts(std::string& error) {
    error.clear();
    std::string api_base;
    std::string device_token;
    if (!LoadConfig(api_base, device_token, error)) {
        return {};
    }

    std::string body;
    int status = 0;
    const std::string url = api_base + "/api/device/v1/podcasts";
    if (!HttpGet(url, device_token, body, status, error)) {
        ESP_LOGW(TAG, "podcast list failed: %s", error.c_str());
        return {};
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "节目列表格式错误";
        return {};
    }
    const cJSON* array = FindPodcastArray(root);
    if (array == nullptr) {
        cJSON_Delete(root);
        error = "节目列表字段缺失";
        return {};
    }

    PodcastList result;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        PodcastItem podcast;
        podcast.id = JsonString(item, "id", "podcast_id");
        podcast.title = JsonString(item, "title");
        podcast.description =
            JsonString(item, "description", "summary");
        podcast.cover_url = JsonString(item, "cover_url", "cover");
        podcast.duration_seconds =
            JsonInt(item, "duration_seconds", "duration");
        podcast.completed = JsonBool(item, "completed", "is_completed");
        if (!podcast.id.empty() && !podcast.title.empty()) {
            result.push_back(std::move(podcast));
        }
    }
    cJSON_Delete(root);
    return result;
}

PodcastPlayInfo PodcastClient::ResolvePlayUrl(const std::string& podcast_id,
                                              std::string& error) {
    error.clear();
    PodcastPlayInfo result;
    {
        std::lock_guard<std::mutex> lock(s_play_cache_mutex);
        const time_t now = time(nullptr);
        for (const auto& entry : s_play_cache) {
            if (entry.id == podcast_id && entry.expires > now) {
                return entry.info;
            }
        }
    }
    std::string api_base;
    std::string device_token;
    if (!LoadConfig(api_base, device_token, error)) {
        result.error = error;
        return result;
    }

    std::string body;
    int status = 0;
    const std::string url = api_base + "/api/device/v1/podcasts/" +
                            UrlEncode(podcast_id) + "/play";
    if (!HttpGet(url, device_token, body, status, error)) {
        result.error = error;
        return result;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "播放地址格式错误";
        result.error = error;
        return result;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        data = root;
    }
    result.url = JsonString(data, "url", "play_url");
    if (result.url.empty()) {
        result.url = JsonString(data, "signed_url");
    }
    result.format = JsonString(data, "format");
    result.expires_at = JsonString(data, "expires_at", "expiresAt");
    cJSON_Delete(root);

    if (result.url.empty()) {
        error = "播放地址为空";
        result.error = error;
        return result;
    }
    result.ok = true;
    {
        std::lock_guard<std::mutex> lock(s_play_cache_mutex);
        const time_t expires = time(nullptr) + kPlayCacheSeconds;
        bool replaced = false;
        for (auto& entry : s_play_cache) {
            if (entry.id == podcast_id) {
                entry.info = result;
                entry.expires = expires;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (s_play_cache.size() >= 8) {
                s_play_cache.erase(s_play_cache.begin());
            }
            s_play_cache.push_back({podcast_id, result, expires});
        }
    }
    return result;
}

std::string PodcastClient::DeviceToken() {
    Settings settings(kSettingsNs, false);
    return settings.GetString(kKeyDeviceToken, CONFIG_PODCAST_DEVICE_TOKEN);
}
