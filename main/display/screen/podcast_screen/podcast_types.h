#pragma once

#include <string>
#include <vector>

struct PodcastItem {
    std::string id;
    std::string title;
    std::string description;
    std::string cover_url;
    int duration_seconds = 0;
    bool completed = false;
};

struct PodcastPlayInfo {
    bool ok = false;
    std::string url;
    std::string format;
    std::string expires_at;
    std::string error;
};

using PodcastList = std::vector<PodcastItem>;
