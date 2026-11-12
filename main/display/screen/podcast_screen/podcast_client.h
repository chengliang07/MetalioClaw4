#pragma once

#include <string>

#include "podcast_types.h"

// 一丢读书 Vercel API 客户端。
//
// NVS namespace "podcast":
//   api_base      https://<vercel-host>
//   device_token  设备 Bearer Token（服务端只保存哈希）
//
// 设备端只持有自己的 Token，不接触 Supabase Service Role Key。
class PodcastClient {
public:
    static PodcastList FetchPodcasts(std::string& error);
    static PodcastPlayInfo ResolvePlayUrl(const std::string& podcast_id,
                                          std::string& error);
    static std::string DeviceToken();
};
