#pragma once

#include "lvgl.h"
#include "screen_util.h"

class PodcastScreen {
public:
    static lv_obj_t* Create(lv_obj_t* return_screen = nullptr);
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
