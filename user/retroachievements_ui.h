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

#endif // RETROACHIEVEMENTS_UI_H