#ifndef RETROACHIEVEMENTS_UI_H
#define RETROACHIEVEMENTS_UI_H

#include "retroachievements_config.h"
#include <psp2/types.h>
#include <vita2d.h>

// Notification system
void trigger_notification(const char* message, unsigned duration_us, vita2d_texture* image);
void draw_notification(void);
void check_and_show_pending_notification(void);

// Achievements menu
void request_show_achievements_menu(void);
void hide_achievements_menu(void);
void show_achievements_menu(void);
void draw_achievements_menu(void);
void ctrl_achievements_menu(void);
void maybe_draw_achievements_menu(void);
int achievements_menu_active(void);

// Menu item management
void menu_reset(void);
void menu_append_item(vita2d_texture* image, const char* title, const char* description, const char* progress);

// Pending notification structure
typedef struct {
    int pending;
    char message[256];
    char image_url[256];
    char cache_name[128];
    int is_badge; // 1 for achievement badge, 0 for avatar
    int is_locked; // for badge only
} PendingNotification;

extern PendingNotification g_pending_notification;

// Leaderboard tracker overlay

typedef struct tracker_data {
    uint32_t id;
    char value[24]; // RC_CLIENT_LEADERBOARD_DISPLAY_SIZE
    int active;
} tracker_data;

void create_tracker(uint32_t id, const char* display);
void destroy_tracker(uint32_t id);
void update_tracker(uint32_t id, const char* display);
tracker_data* find_tracker(uint32_t id);
void draw_leaderboard_trackers(void);
void draw_challenge_indicators(void);
void create_challenge_indicator(uint32_t id, vita2d_texture* image);
void destroy_challenge_indicator(uint32_t id);

// Progress indicator overlay

typedef struct progress_indicator_data {
    int active;
    char title[128];
    char description[128];
    char progress[64];
    float percent; // 0.0 to 100.0
    SceUInt64 until; // time to hide (sceKernelGetProcessTimeWide)
    vita2d_texture* image;
} progress_indicator_data;

void show_progress_indicator(const char* title, const char* description, const char* progress, float percent, vita2d_texture* image, unsigned duration_us);
void update_progress_indicator(const char* progress, float percent);
void hide_progress_indicator(void);
void draw_progress_indicator(void);

#endif // RETROACHIEVEMENTS_UI_H