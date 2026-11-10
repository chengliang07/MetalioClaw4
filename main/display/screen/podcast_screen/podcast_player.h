#pragma once

#include <string>

enum class PodcastPlayerEvent {
    Playing,
    Paused,
    Finished,
    Error,
};

using PodcastPlayerEventCallback =
    void (*)(PodcastPlayerEvent event, const char* detail, void* ctx);

class PodcastPlayer {
public:
    static bool Start(const std::string& url, std::string& error);
    static bool Pause();
    static bool Resume();
    static void Stop();
    static void SetEventCallback(PodcastPlayerEventCallback callback,
                                 void* ctx);
};
