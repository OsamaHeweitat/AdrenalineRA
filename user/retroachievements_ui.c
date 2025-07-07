#include "retroachievements_config.h"
#include "retroachievements.h"
#include "retroachievements_ui.h"
#include <rc_client.h>
#include "menu.h"
#include "utils.h"
#include <vita2d.h>

static char g_notification_msg[128] = {0};
static SceUInt64 g_notification_until = 0;
static vita2d_texture* g_notification_image = NULL; // Optional

static int g_show_achievements_menu = 0;
static int g_achievements_menu_sel = 0; // Currently selected item (for highlight)

PendingNotification g_pending_notification = {0};

#define MAX_MENU_ITEMS 128
#define MAX_MENU_TEXT 128

typedef struct MenuItem {
    vita2d_texture* image;
    char title[MAX_MENU_TEXT];
    char description[MAX_MENU_TEXT];
    char progress[MAX_MENU_TEXT];
    int is_header; // 1 if this is a header/category label
} MenuItem;

static struct {
    MenuItem items[MAX_MENU_ITEMS];
    int count;
} g_menu = {0};

// Call this from any thread/context to trigger a notification (with optional image)
void trigger_vita2d_notification(const char* message, unsigned duration_us, vita2d_texture* image) {
    strncpy(g_notification_msg, message, sizeof(g_notification_msg)-1);
    g_notification_msg[sizeof(g_notification_msg)-1] = '\0';
    g_notification_until = sceKernelGetProcessTimeWide() + duration_us;
    g_notification_image = image; // Set image (can be NULL)
}

void draw_vita2d_notification(void) {
    if (g_notification_msg[0] && sceKernelGetProcessTimeWide() < g_notification_until) {
        sceClibPrintf("[RA DEBUG] Drawing notification: %s\n", g_notification_msg);
        float notif_width = 600.0f;
        float notif_height = 60.0f;
        float notif_x = (960.0f - notif_width) / 2.0f;
        float notif_y = 544.0f - notif_height - 40.0f;
        vita2d_draw_rectangle(notif_x, notif_y, notif_width, notif_height, 0xC0000000);
        float image_size = 48.0f;
        float image_x = notif_x + 8.0f;
        float image_y = notif_y + (notif_height - image_size) / 2.0f;
        float text_x = notif_x + 16.0f + image_size;
        float text_y = notif_y + 40.0f;
        if (g_notification_image) {
            vita2d_draw_texture_scale(g_notification_image, image_x, image_y, image_size / vita2d_texture_get_width(g_notification_image), image_size / vita2d_texture_get_height(g_notification_image));
        } else {
            text_x = notif_x + 24.0f;
        }
        float text_width = vita2d_pgf_text_width(font, 1.0f, g_notification_msg);
        // If image, align text left of image; else, center text
        if (!g_notification_image) {
            text_x = notif_x + (notif_width - text_width) / 2.0f;
        }
        vita2d_pgf_draw_text(font, text_x, text_y, 0xFFFFFFFF, 1.0f, g_notification_msg);
    } else if (g_notification_msg[0]) {
        g_notification_msg[0] = 0;
        g_notification_image = NULL;
    }
}

void draw_achievements_menu() {
    // sceClibPrintf("[RA DEBUG] drawAchievementsMenu: start\n");

    if (!font) {
        sceClibPrintf("[RA DEBUG] drawAchievementsMenu: font is NULL! Aborting draw.\n");
        return;
    }
    // sceClibPrintf("[RA DEBUG] drawAchievementsMenu: font pointer = %p\n", font);

    // Draw window background
    float x = 80.0f, y = 40.0f, width = 800.0f, height = 464.0f;
    // sceClibPrintf("[RA DEBUG] drawAchievementsMenu: about to draw window background\n"); 
    vita2d_draw_rectangle(x, y, width, height, 0xE0000000);

    // Draw title bar
    float title_height = 38.0f;
    // sceClibPrintf("[RA DEBUG] drawAchievementsMenu: about to draw title bar\n"); 
    vita2d_draw_rectangle(x, y, width, title_height, 0xFF444444);
    const char *title = "Achievements";
    float title_width = vita2d_pgf_text_width(font, 1.5f, title);
    // sceClibPrintf("[RA DEBUG] drawAchievementsMenu: title_width = %f\n", title_width);
    vita2d_pgf_draw_text(font, x + (width - title_width) / 2.0f, y + 28.0f, 0xFFFFFFFF, 1.5f, title);

    // Draw menu items
    float item_y = y + title_height + 10.0f;
    float item_height = 56.0f;
    float image_size = 48.0f;
    float padding = 12.0f;
    int visible_index = 0;
    int highlight_index = g_achievements_menu_sel;
    int first_visible = 0;
    int max_visible = (int)((height - title_height - 20.0f) / (item_height + 4.0f));
    // Find first visible item for scrolling
    if (highlight_index >= max_visible) {
        first_visible = highlight_index - max_visible + 1;
    }
    for (int i = 0; i < g_menu.count; ++i) {
        if (i < first_visible) continue;
        MenuItem *item = &g_menu.items[i];
        if (item->is_header) {
            vita2d_draw_rectangle(x + padding, item_y, width - 2 * padding, 32.0f, 0xFF222244);
            vita2d_pgf_draw_text(font, x + padding + 8.0f, item_y + 24.0f, 0xFFAAAAFF, 1.2f, item->description[0] ? item->description : "");
            item_y += 36.0f;
        } else {
            // Highlight if selected
            if (i == highlight_index) {
                vita2d_draw_rectangle(x + padding, item_y, width - 2 * padding, item_height, 0xFFFF1F7F); // Highlight color
            } else {
                vita2d_draw_rectangle(x + padding, item_y, width - 2 * padding, item_height, 0x40000000);
            }
            float text_x = x + padding + image_size + 16.0f;
            float text_y = item_y + 24.0f;
            if (item->image) {
                vita2d_draw_texture_scale(item->image, x + padding, item_y + 4.0f, image_size / vita2d_texture_get_width(item->image), image_size / vita2d_texture_get_height(item->image));
            }
            vita2d_pgf_draw_text(font, text_x, text_y, 0xFFFFFFFF, 1.0f, item->title);
            if (item->description[0]) {
                vita2d_pgf_draw_text(font, text_x, text_y + 18.0f, 0xFFCCCCCC, 0.85f, item->description);
            }
            if (item->progress[0]) {
                float progress_width = vita2d_pgf_text_width(font, 0.9f, item->progress);
                vita2d_pgf_draw_text(font, x + width - padding - progress_width, text_y, 0xFF00FF00, 0.9f, item->progress);
            }
            item_y += item_height + 4.0f;
        }
        visible_index++;
        if (visible_index >= max_visible) {
            break;
        }
    }
    // sceClibPrintf("[RA DEBUG] drawAchievementsMenu: end\n");
}

// Call this to request showing the achievements menu
void request_show_achievements_menu(void) {
    g_show_achievements_menu = 1;
    g_achievements_menu_sel = 0; // Reset selection to first item
    // show_achievements_menu(); // Only populates the menu, does not draw
}

// Call this to hide the achievements menu
void hide_achievements_menu(void) {
    g_show_achievements_menu = 0;
}

// Input handler for achievements menu
void ctrl_achievements_menu() {
    extern Pad pressed_pad, released_pad, hold_pad;
    if (released_pad[PAD_CANCEL] || released_pad[PAD_PSBUTTON]) {
        hide_achievements_menu();
        return;
    }
    // Find next/prev selectable (non-header) item
    int prev_sel = g_achievements_menu_sel;
    if (hold_pad[PAD_UP] || hold_pad[PAD_LEFT_ANALOG_UP]) {
        int i = g_achievements_menu_sel - 1;
        while (i >= 0) {
            if (!g_menu.items[i].is_header) {
                g_achievements_menu_sel = i;
                break;
            }
            i--;
        }
    }
    if (hold_pad[PAD_DOWN] || hold_pad[PAD_LEFT_ANALOG_DOWN]) {
        int i = g_achievements_menu_sel + 1;
        while (i < g_menu.count) {
            if (!g_menu.items[i].is_header) {
                g_achievements_menu_sel = i;
                break;
            }
            i++;
        }
    }
    // POTENTIAL: handle PAD_ENTER for future actions (view details, etc)
}

// Call this from the main rendering loop (AKA menu.c)
void maybe_draw_achievements_menu(void) {
    if (g_show_achievements_menu) {
        draw_achievements_menu();
    }
}

// also call this from the main loop to block game input and handle achievements menu
int achievements_menu_active(void) {
    return g_show_achievements_menu;
}

void show_achievements_menu(void)
{
  sceClibPrintf("[RA DEBUG] show_achievements_menu: start\n");
  char url[128];
  const char* progress;

  // This will return a list of lists. Each top-level item is an achievement category
  // (Active Challenge, Unlocked, etc). Empty categories are not returned, so we can
  // just display everything that is returned.
  rc_client_achievement_list_t* list = rc_client_create_achievement_list(g_client,
      RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
      RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  sceClibPrintf("[RA DEBUG] show_achievements_menu: created achievement list %p\n", list);

  // Clear any previously loaded menu items
  menu_reset();
  sceClibPrintf("[RA DEBUG] show_achievements_menu: menu_reset done\n");

  for (int i = 0; i < list->num_buckets; i++)
  {
    sceClibPrintf("[RA DEBUG] show_achievements_menu: bucket %d label='%s', num_achievements=%d\n", i, list->buckets[i].label, list->buckets[i].num_achievements);
    // Create a header item for the achievement category
    menu_append_item(NULL, NULL, list->buckets[i].label, NULL);

    for (int j = 0; j < list->buckets[i].num_achievements; j++)
    {
      const rc_client_achievement_t* achievement = list->buckets[i].achievements[j];
      vita2d_texture* image_data = NULL;
      // sceClibPrintf("[RA DEBUG] show_achievements_menu: bucket %d, achievement %d, title='%s'\n", i, j, achievement->title);

      if (rc_client_achievement_get_image_url(achievement, achievement->state, url, sizeof(url)) == RC_OK)
      {
         // Generate a local filename to store the downloaded image.
         char achievement_badge[64];
         snprintf(achievement_badge, sizeof(achievement_badge), "ach_%s%s.png", achievement->badge_name, (achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) ? "" : "_lock");
         //  sceClibPrintf("[RA DEBUG] show_achievements_menu: downloading badge '%s' from url '%s'\n", achievement_badge, url);
         image_data = download_achievement_badge(url, achievement_badge, achievement->state != RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
         //  sceClibPrintf("[RA DEBUG] show_achievements_menu: image_data=%p\n", image_data);
      }

      // Determine the "progress" of the achievement. This can also be used to show
      // locked/unlocked icons and progress bars.
      if (list->buckets[i].bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED)
        progress = "Unsupported";
      else if (achievement->unlocked)
        progress = "Unlocked";
      else if (achievement->measured_percent)
        progress = achievement->measured_progress;
      else
        progress = "Locked";

      // sceClibPrintf("[RA DEBUG] show_achievements_menu: appending item title='%s', desc='%s', progress='%s'\n", achievement->title, achievement->description, progress);
      menu_append_item(image_data, achievement->title, achievement->description, progress);
    }
  }

  // drawAchievementsMenu();
  request_show_achievements_menu();
  sceClibPrintf("[RA DEBUG] show_achievements_menu: destroying achievement list\n");
  rc_client_destroy_achievement_list(list);
  sceClibPrintf("[RA DEBUG] show_achievements_menu: end\n");
}

void menu_reset(void) {
    for (int i = 0; i < g_menu.count; ++i) {
        if (g_menu.items[i].image) {
            vita2d_free_texture(g_menu.items[i].image);
            g_menu.items[i].image = NULL;
        }
    }
    g_menu.count = 0;
}

// Add a menu item. If title is NULL, it's a header/category label.
void menu_append_item(vita2d_texture* image, const char* title, const char* description, const char* progress) {
    if (g_menu.count >= MAX_MENU_ITEMS)
        return;
    MenuItem* item = &g_menu.items[g_menu.count++];
    item->image = image;
    if (title) {
        strncpy(item->title, title, MAX_MENU_TEXT-1);
        item->title[MAX_MENU_TEXT-1] = '\0';
        item->is_header = 0;
    } else {
        item->title[0] = '\0';
        item->is_header = 1;
    }
    if (description) {
        strncpy(item->description, description, MAX_MENU_TEXT-1);
        item->description[MAX_MENU_TEXT-1] = '\0';
    } else {
        item->description[0] = '\0';
    }
    if (progress) {
        strncpy(item->progress, progress, MAX_MENU_TEXT-1);
        item->progress[MAX_MENU_TEXT-1] = '\0';
    } else {
        item->progress[0] = '\0';
    }
}

void check_and_show_pending_notification(void) {
    if (g_pending_notification.pending) {
        vita2d_texture* image = NULL;
        if (g_pending_notification.image_url[0]) {
            if (g_pending_notification.is_badge) {
                image = download_image_texture(g_pending_notification.image_url, g_pending_notification.cache_name);
            } else {
                image = download_image_texture(g_pending_notification.image_url, g_pending_notification.cache_name);
            }
        }
        trigger_vita2d_notification(g_pending_notification.message, 3000000, image);
        g_pending_notification.pending = 0;
    }
}

// --- Leaderboard Tracker Overlay ---
#define MAX_TRACKERS 4
static tracker_data g_trackers[MAX_TRACKERS] = {0};

void create_tracker(uint32_t id, const char* display) {
    sceClibPrintf("[RA DEBUG] create_tracker called\n");

    // Find existing or free slot
    for (int i = 0; i < MAX_TRACKERS; ++i) {
        if (g_trackers[i].active && g_trackers[i].id == id) {
            strncpy(g_trackers[i].value, display, sizeof(g_trackers[i].value)-1);
            g_trackers[i].value[sizeof(g_trackers[i].value)-1] = '\0';
            return;
        }
    }
    for (int i = 0; i < MAX_TRACKERS; ++i) {
        if (!g_trackers[i].active) {
            g_trackers[i].id = id;
            strncpy(g_trackers[i].value, display, sizeof(g_trackers[i].value)-1);
            g_trackers[i].value[sizeof(g_trackers[i].value)-1] = '\0';
            g_trackers[i].active = 1;
            return;
        }
    }
    // No free slot: do nothing
}

void destroy_tracker(uint32_t id) {
    sceClibPrintf("[RA DEBUG] destroy_tracker called\n");

    for (int i = 0; i < MAX_TRACKERS; ++i) {
        if (g_trackers[i].active && g_trackers[i].id == id) {
            g_trackers[i].active = 0;
            g_trackers[i].id = 0;
            g_trackers[i].value[0] = '\0';
            return;
        }
    }
}

tracker_data* find_tracker(uint32_t id) {
    sceClibPrintf("[RA DEBUG] find_tracker called\n");

    for (int i = 0; i < MAX_TRACKERS; ++i) {
        if (g_trackers[i].active && g_trackers[i].id == id) {
            return &g_trackers[i];
        }
    }
    return NULL;
}

void draw_leaderboard_trackers(void) {
    // Draw all active trackers as overlay boxes at the top of the screen
    float x = 40.0f;
    float y = 10.0f;
    float width = 320.0f;
    float height = 38.0f;
    float spacing = 8.0f;
    int count = 0;
    for (int i = 0; i < MAX_TRACKERS; ++i) {
        if (g_trackers[i].active) {
            float box_y = y + count * (height + spacing);
            vita2d_draw_rectangle(x, box_y, width, height, 0xE0000000);
            // Draw tracker value (fixed-width font recommended)
            if (font) {
                vita2d_pgf_draw_text(font, x + 16.0f, box_y + 28.0f, 0xFFFFFFFF, 1.2f, g_trackers[i].value);
            }
            count++;
        }
    }
}

// --- Progress Indicator Overlay ---
static progress_indicator_data g_progress_indicator = {0};

void show_progress_indicator(const char* title, const char* description, const char* progress, float percent, unsigned duration_us) {
    strncpy(g_progress_indicator.title, title, sizeof(g_progress_indicator.title)-1);
    g_progress_indicator.title[sizeof(g_progress_indicator.title)-1] = '\0';
    strncpy(g_progress_indicator.description, description, sizeof(g_progress_indicator.description)-1);
    g_progress_indicator.description[sizeof(g_progress_indicator.description)-1] = '\0';
    strncpy(g_progress_indicator.progress, progress, sizeof(g_progress_indicator.progress)-1);
    g_progress_indicator.progress[sizeof(g_progress_indicator.progress)-1] = '\0';
    g_progress_indicator.percent = percent;
    g_progress_indicator.active = 1;
    g_progress_indicator.until = sceKernelGetProcessTimeWide() + duration_us;
}

void update_progress_indicator(const char* progress, float percent) {
    if (!g_progress_indicator.active)
        return;
    if (progress) {
        strncpy(g_progress_indicator.progress, progress, sizeof(g_progress_indicator.progress)-1);
        g_progress_indicator.progress[sizeof(g_progress_indicator.progress)-1] = '\0';
    }
    g_progress_indicator.percent = percent;
}

void hide_progress_indicator(void) {
    g_progress_indicator.active = 0;
}

void draw_progress_indicator(void) {
    if (!g_progress_indicator.active)
        return;
    if (sceKernelGetProcessTimeWide() > g_progress_indicator.until) {
        g_progress_indicator.active = 0;
        return;
    }
    float x = 180.0f;
    float y = 20.0f;
    float width = 600.0f;
    float height = 120.0f;
    // Background
    vita2d_draw_rectangle(x, y, width, height, 0xE0000000);
    // Title
    if (font) {
        vita2d_pgf_draw_text(font, x + 32.0f, y + 38.0f, 0xFFFFFFFF, 1.3f, g_progress_indicator.title);
        vita2d_pgf_draw_text(font, x + 32.0f, y + 68.0f, 0xFFCCCCCC, 1.0f, g_progress_indicator.description);
        vita2d_pgf_draw_text(font, x + 32.0f, y + 98.0f, 0xFF00FF00, 1.0f, g_progress_indicator.progress);
    }
}