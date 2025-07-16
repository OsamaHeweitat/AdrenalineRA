#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "retroachievements.h"
#include <rc_client.h>
#include <rc_consoles.h>
#include <rc_hash.h>

#include <psp2/kernel/clib.h> 
#include <psp2/sysmodule.h>
#include <psp2/notificationutil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>

#include "main.h"
#include "menu.h"
#include "../adrenaline_compat.h"
#include <ctype.h>

#include <vita2d.h>

#include <psp2/types.h>

#include "utils.h"
#include "virtualsfo.h"

#include "retroachievements_ui.h"
#include "retroachievements_cache.h"
#include "retroachievements_iso_parser.h"
#include "retroachievements_network.h"

rc_client_t* g_client = NULL;

// Structure to hold async callback data
typedef struct {
    rc_client_server_callback_t callback;
    void* callback_data;
} async_callback_data;

char ISO_DIR[256];
char GAME_DIR[256];
char CACHE_FILE[256];
char CACHE_DIR[256];
char CREDENTIALS_FILE[256];

#define MAX_TITLEID 64
#define MAX_PATH 256
#define MAX_CACHE_ENTRIES 1024

int show_message(const char* message, ...)
{
  sceClibPrintf(message);

  return 0;
}

// This is the function the rc_client will use to read memory for the emulator. we don't need it yet,
// so just provide a dummy function that returns "no memory read".
static uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    // PSP RAM is 64MB, mapped at 0x88000000 in the emulator
    if (address >= PSP_RAM_SIZE || address + num_bytes > PSP_RAM_SIZE)
        return 0;
    void *ram = (void *)ScePspemuConvertAddress(0x88000000, KERMIT_INPUT_MODE, PSP_RAM_SIZE);
    memcpy(buffer, (uint8_t*)ram + address, num_bytes);
    return num_bytes;
}

// This is the callback function for the asynchronous HTTP call (which is not provided in this example)
static void http_callback(int status_code, const char* content, size_t content_size, void* userdata, const char* error_message)
{
  // Prepare a data object to pass the HTTP response to the callback
  rc_api_server_response_t server_response;
  memset(&server_response, 0, sizeof(server_response));
  server_response.body = content;
  server_response.body_length = content_size;
  server_response.http_status_code = status_code;

  // handle non-http errors (socket timeout, no internet available, etc)
  if (status_code == 0 && error_message) {
      // assume no server content and pass the error through instead
      server_response.body = error_message;
      server_response.body_length = strlen(error_message);
      // Let rc_client know the error was not catastrophic and could be retried. It may decide to retry or just 
      // immediately pass the error to the callback. To prevent possible retries, use RC_API_SERVER_RESPONSE_CLIENT_ERROR.
      server_response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  }

  // sceClibPrintf("[RA DEBUG] http_callback: status=%d\n", server_response.http_status_code);
  // sceClibPrintf("[RA DEBUG] http_callback: body_length=%d\n", (int)server_response.body_length);
  // sceClibPrintf("[RA DEBUG] http_callback: body=%s\n", server_response.body);

  // Get the rc_client callback and call it
  async_callback_data* async_data = (async_callback_data*)userdata;
  async_data->callback(&server_response, async_data->callback_data);

  // Release the captured rc_client callback data
  free(async_data);
}

// This is the HTTP request dispatcher that is provided to the rc_client. Whenever the client
// needs to talk to the server, it will call this function.
static void server_call(const rc_api_request_t* request,
  rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{

  // RetroAchievements may not allow hardcore unlocks if we don't properly identify ourselves.
  const char* user_agent = "Adrenaline/1.0-debug (PSVita)";
  
  // callback must be called with callback_data, regardless of the outcome of the HTTP call.
  // Since we're making the HTTP call asynchronously, we need to capture them and pass it
  // through the async HTTP code.
  async_callback_data* async_data = malloc(sizeof(async_callback_data));
  async_data->callback = callback;
  async_data->callback_data = callback_data;

  // If post data is provided, we need to make a POST request, otherwise, a GET request will suffice.
  if (request->post_data)
    async_http_post(request->url, request->post_data, user_agent, http_callback, async_data, request->content_type);
  else
    async_http_get(request->url, user_agent, http_callback, async_data);
}

// Write log messages to the console
static void log_message(const char* message, const rc_client_t* client)
{
  printf("%s\n", message);
  sceClibPrintf("%s\n", message);
}

void shutdown_retroachievements_client(void)
{
  if (g_client)
  {
    // Release resources associated to the client instance
    rc_client_destroy(g_client);
    g_client = NULL;
  }  
}

extern vita2d_pgf *font;

static void login_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
    if (result != RC_OK) {
        sceClibPrintf("Login failed: %s\n", error_message);
        return;
    }
    const rc_client_user_t* user = rc_client_get_user_info(client);
    sceClibPrintf("[RA DEBUG] login_callback: user pointer=%p\n", user);
    if (user) {
        sceClibPrintf("[RA DEBUG] login_callback: username=%s\n", user->username);
        sceClibPrintf("[RA DEBUG] login_callback: display_name=%s\n", user->display_name);
        sceClibPrintf("[RA DEBUG] login_callback: score=%u\n", user->score);
        sceClibPrintf("[RA DEBUG] login_callback: avatar_url=%s\n", user->avatar_url ? user->avatar_url : "NULL");
    } else {
        sceClibPrintf("[RA DEBUG] login_callback: user is NULL\n");
    }
    store_retroachievements_credentials(user->username, user->token);
    char login_msg[128];
    snprintf(login_msg, sizeof(login_msg), "Logged in as %s (%u points)", user->display_name, user->score);
    if (user && user->avatar_url) {
        g_pending_notification.pending = 1;
        snprintf(g_pending_notification.message, sizeof(g_pending_notification.message), "%s", login_msg);
        snprintf(g_pending_notification.image_url, sizeof(g_pending_notification.image_url), "%s", user->avatar_url);
        snprintf(g_pending_notification.cache_name, sizeof(g_pending_notification.cache_name), "avatar_%s", strrchr(user->avatar_url, '/') ? strrchr(user->avatar_url, '/') + 1 : "default.jpg");
        g_pending_notification.is_badge = 0;
        g_pending_notification.is_locked = 0;
    } else {
        g_pending_notification.pending = 1;
        snprintf(g_pending_notification.message, sizeof(g_pending_notification.message), "%s", login_msg);
        g_pending_notification.image_url[0] = '\0';
        g_pending_notification.cache_name[0] = '\0';
        g_pending_notification.is_badge = 0;
        g_pending_notification.is_locked = 0;
    }
}

void login_retroachievements_user(const char* username, const char* password)
{
  // This will generate an HTTP payload and call the server_call chain above.
  // Eventually, login_callback will be called to let us know if the login was successful.
  rc_client_begin_login_with_password(g_client, username, password, login_callback, NULL);
}

void login_remembered_retroachievements_user(const char* username, const char* token)
{
  // This is exactly the same functionality as rc_client_begin_login_with_password, but
  // uses the token captured from the first login instead of a password.
  // Note that it uses the same callback.
  rc_client_begin_login_with_token(g_client, username, token, login_callback, NULL);
}

static void show_game_placard(void)
{
  char message[128], url[128];
  // async_image_data* image_data = NULL;
  vita2d_texture* image_data = NULL;
  const rc_client_game_t* game = rc_client_get_game_info(g_client);
  rc_client_user_game_summary_t summary;
  rc_client_get_user_game_summary(g_client, &summary);

  // Construct a message indicating the number of achievements unlocked by the user.
  if (summary.num_core_achievements == 0)
  {
    snprintf(message, sizeof(message), "This game has no achievements.");
  }
  else if (summary.num_unsupported_achievements)
  {
    snprintf(message, sizeof(message), "You have %u of %u achievements unlocked (%d unsupported).",
        summary.num_unlocked_achievements, summary.num_core_achievements,
        summary.num_unsupported_achievements);
  }
  else
  {
    snprintf(message, sizeof(message), "You have %u of %u achievements unlocked.",
        summary.num_unlocked_achievements, summary.num_core_achievements);
  }

  // The emulator is responsible for managing images. This uses rc_client to build
  // the URL where the image should be downloaded from.
  if (rc_client_game_get_image_url(game, url, sizeof(url)) == RC_OK)
  {
    // Generate a local filename to store the downloaded image.
    char game_badge[64];
    snprintf(game_badge, sizeof(game_badge), "game_%s.png", game->badge_name);

    // This function will download and cache the game image. It is up to the emulator
    // to implement this logic. Similarly, the emulator has to use image_data to
    // display the game badge in the placard, or a placeholder until the image is
    // downloaded. None of that logic is provided in this example.
    // image_data = download_and_cache_image(game_badge, url);
    image_data = download_game_icon(url, game->badge_name);
  } 

  // show_popup_message(image_data, game->title, message);
  trigger_vita2d_notification(message, 3000000, image_data);
}

static void load_game_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  if (result != RC_OK)
  {
    show_message("RetroAchievements game load failed: %s", error_message);
    return;
  }

  // announce that the game is ready. we'll cover this in the next section.
  show_game_placard();
}

void load_game(const uint8_t* rom, size_t rom_size)
{
  sceClibPrintf("[RA DEBUG] load_game called\n");
  sceClibPrintf("[RA DEBUG] ROM pointer: %p, size: %u\n", rom, (unsigned int)rom_size);
  if (!g_client) {
    sceClibPrintf("[RA DEBUG] g_client is NULL in load_game!\n");
    return;
  }
  sceClibPrintf("[RA DEBUG] Calling rc_client_begin_identify_and_load_game (memory)\n");
  rc_client_begin_identify_and_load_game(g_client, RC_CONSOLE_PSP, NULL, rom, rom_size, load_game_callback, NULL);
}

void load_game_from_file(const char* path)
{
  sceClibPrintf("[RA DEBUG] load_game_from_file called\n");
  if (!path) {
    sceClibPrintf("[RA DEBUG] load_game_from_file: path is NULL!\n");
    return;
  }
  sceClibPrintf("[RA DEBUG] Path: %s\n", path);
  if (!g_client) {
    sceClibPrintf("[RA DEBUG] g_client is NULL in load_game_from_file!\n");
    return;
  }
  // Check if file exists with stat
  SceIoStat stat;
  if (sceIoGetstat(path, &stat) < 0) {
    sceClibPrintf("[RA DEBUG] File does not exist: %s\n", path);
    return;
  }
  sceClibPrintf("[RA DEBUG] File exists: %s\n", path);
  sceClibPrintf("[RA DEBUG] Calling rc_client_begin_identify_and_load_game (file) with path: %s\n", path);
  rc_client_begin_identify_and_load_game(g_client, RC_CONSOLE_PSP, path, NULL, 0, load_game_callback, NULL);
}

static void achievement_triggered(const rc_client_achievement_t* achievement)
{
    char url[128];
    if (rc_client_achievement_get_image_url(achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url)) == RC_OK) {
        g_pending_notification.pending = 1;
        snprintf(g_pending_notification.message, sizeof(g_pending_notification.message),
            "Achievement Unlocked: %s (%d)", achievement->title, achievement->points);
        snprintf(g_pending_notification.image_url, sizeof(g_pending_notification.image_url), "%s", url);
        snprintf(g_pending_notification.cache_name, sizeof(g_pending_notification.cache_name), "badge_%s.png", achievement->badge_name);
        g_pending_notification.is_badge = 1;
        g_pending_notification.is_locked = (achievement->state != RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
    } else {
        g_pending_notification.pending = 1;
        snprintf(g_pending_notification.message, sizeof(g_pending_notification.message),
            "Achievement Unlocked: %s (%d)", achievement->title, achievement->points);
        g_pending_notification.image_url[0] = '\0';
        g_pending_notification.cache_name[0] = '\0';
        g_pending_notification.is_badge = 1;
        g_pending_notification.is_locked = 0;
    }
}

static void leaderboard_started(const rc_client_leaderboard_t* leaderboard)
{
  sceClibPrintf("[RA DEBUG] leaderboard_started called\n");
  show_message("Leaderboard attempt started: %s - %s", leaderboard->title, leaderboard->description);
}

static void leaderboard_failed(const rc_client_leaderboard_t* leaderboard)
{
  sceClibPrintf("[RA DEBUG] leaderboard_failed called\n");
  show_message("Leaderboard attempt failed: %s", leaderboard->title);
}

static void leaderboard_submitted(const rc_client_leaderboard_t* leaderboard)
{
  sceClibPrintf("[RA DEBUG] leaderboard_submitted called\n");
  show_message("Submitted %s for %s", leaderboard->tracker_value, leaderboard->title);
}

static void leaderboard_tracker_update(const rc_client_leaderboard_tracker_t* tracker)
{
  sceClibPrintf("[RA DEBUG] leaderboard_tracker_update called\n");
  // Find the currently visible tracker by ID and update what's being displayed.
  tracker_data* data = find_tracker(tracker->id);
  if (data)
  {
    // The display text buffer is guaranteed to live for as long as the game is loaded,
    // but it may be updated in a non-thread safe manner within rc_client_do_frame, so
    // we create a copy for the rendering code to read.
    strcpy(data->value, tracker->display);
  }
}

static void leaderboard_tracker_show(const rc_client_leaderboard_tracker_t* tracker)
{
  sceClibPrintf("[RA DEBUG] leaderboard_tracker_show called\n");
  // The actual implementation of converting an rc_client_leaderboard_tracker_t to
  // an on-screen widget is going to be client-specific. The provided tracker object
  // has a unique identifier for the tracker and a string to be displayed on-screen.
  // The string should be displayed using a fixed-width font to eliminate jittering
  // when timers are updated several times a second.
  create_tracker(tracker->id, tracker->display);
}

static void leaderboard_tracker_hide(const rc_client_leaderboard_tracker_t* tracker)
{
  sceClibPrintf("[RA DEBUG] leaderboard_tracker_hide called\n");
  // This tracker is no longer needed
  destroy_tracker(tracker->id);
}

static void progress_indicator_update(const rc_client_achievement_t* achievement)
{
    // Update the progress indicator overlay with new progress string and percent
    update_progress_indicator(achievement->measured_progress, achievement->measured_percent);
}

static void progress_indicator_show(const rc_client_achievement_t* achievement)
{
    // Show the progress indicator overlay with achievement info
    show_progress_indicator(
        achievement->title,
        achievement->description,
        achievement->measured_progress,
        achievement->measured_percent,
        2000000 // 2 seconds
    );
}

static void progress_indicator_hide(void)
{
    // Hide the progress indicator overlay
    hide_progress_indicator();
}

static void game_mastered(void)
{
  char message[128], submessage[128];
  char url[128];
  // async_image_data* image_data = NULL;
  vita2d_texture* image_data = NULL;
  const rc_client_game_t* game = rc_client_get_game_info(g_client);

  if (rc_client_game_get_image_url(game, url, sizeof(url)) == RC_OK)
  {
    // Generate a local filename to store the downloaded image.
    char game_badge[64];
    snprintf(game_badge, sizeof(game_badge), "game_%s.png", game->badge_name); 
    image_data = download_game_icon(url, game->badge_name);
  }

  // The popup should say "Completed GameTitle" or "Mastered GameTitle",
  // depending on whether or not hardcore is enabled.
  snprintf(message, sizeof(message), "%s %s", 
      rc_client_get_hardcore_enabled(g_client) ? "Mastered" : "Completed",
      game->title);

  // You should also display the name of the user (for personalized screenshots).
  // If the emulator keeps track of the user's per-game playtime, it's nice to
  // display that too.
  snprintf(submessage, sizeof(submessage), "%s", rc_client_get_user_info(g_client)->display_name);

  strncat(message, " - ", sizeof(message) - strlen(message) - 1);
  strncat(message, submessage, sizeof(message) - strlen(message) - 1);

  // show_popup_message(image_data, message, submessage);
  trigger_vita2d_notification(message, 3000000, image_data);

  // play_sound("mastery.wav");
}

static void event_handler(const rc_client_event_t* event, rc_client_t* client)
{
  sceClibPrintf("[RA DEBUG] event_handler called\n");
  switch (event->type)
  {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED called\n");
      achievement_triggered(event->achievement);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_LEADERBOARD_STARTED called\n");
      leaderboard_started(event->leaderboard);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_LEADERBOARD_FAILED called\n");
      leaderboard_failed(event->leaderboard);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED called\n");
      leaderboard_submitted(event->leaderboard);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE called\n");
      leaderboard_tracker_update(event->leaderboard_tracker);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW called\n");
      leaderboard_tracker_show(event->leaderboard_tracker);
      break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE called\n");
      leaderboard_tracker_hide(event->leaderboard_tracker);
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW called\n");
      progress_indicator_show(event->achievement);
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE called\n");
      progress_indicator_update(event->achievement);
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
      sceClibPrintf("[RA DEBUG] RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE called\n");
      progress_indicator_hide();
      break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
      game_mastered();
      break;
    default:
      sceClibPrintf("[RA DEBUG] Unhandled event %d\n", event->type);
      break;
  }
}

void initialize_retroachievements_client(void)
{
  // Create the client instance (using a global variable simplifies this example)
  g_client = rc_client_create(read_memory, server_call);

  // Provide a logging function to simplify debugging
  rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_VERBOSE, log_message);

  rc_client_set_event_handler(g_client, event_handler);

  // Disable hardcore - if we goof something up in the implementation, we don't want our
  // account disabled for cheating.
  rc_client_set_hardcore_enabled(g_client, 0);
}

int start() {
  sceClibPrintf("we're trying net init\n");
  net_init();
  sceClibPrintf("we're trying http init\n");
  http_init();
  sceClibPrintf("we're trying ssl init\n");
  ssl_init();

  if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) == SCE_SYSMODULE_LOADED) {
    sceClibPrintf("Net module loaded\n");
  } else {
    sceClibPrintf("Net module not loaded\n");
  }

  if (sceSysmoduleIsLoaded(SCE_SYSMODULE_HTTPS) == SCE_SYSMODULE_LOADED) {
    sceClibPrintf("HTTPS module loaded\n");
  } else {
    sceClibPrintf("HTTPS module not loaded\n");
  }

  if (sceSysmoduleIsLoaded(SCE_SYSMODULE_SSL) == SCE_SYSMODULE_LOADED) {
    sceClibPrintf("SSL module loaded\n");
  } else {
    sceClibPrintf("SSL module not loaded\n");
  }

  snprintf(ISO_DIR, sizeof(ISO_DIR), "%s/ISO", getPspemuMemoryStickLocation());
  snprintf(GAME_DIR, sizeof(GAME_DIR), "%s/PSP/GAME", getPspemuMemoryStickLocation());
  snprintf(CACHE_FILE, sizeof(CACHE_FILE), "%s/ra_titleid_cache.txt", getPspemuMemoryStickLocation());
  snprintf(CACHE_DIR, sizeof(CACHE_DIR), "%s/ra_cache", getPspemuMemoryStickLocation());
  snprintf(CREDENTIALS_FILE, sizeof(CREDENTIALS_FILE), "%s/ra_credentials.txt", getPspemuMemoryStickLocation());

  // Check if user updated credentials from menu and update file if so
  update_credentials_from_menu();

  initialize_retroachievements_client();

  char username[128];
  char token[128];
  if (load_retroachievements_credentials(username, sizeof(username), token, sizeof(token)) == 0) {
    sceClibPrintf("[RA DEBUG] Logging in with stored credentials\n");
    login_remembered_retroachievements_user(username, token);
  } else if (load_retroachievements_credentials(username, sizeof(username), token, sizeof(token)) == 1) {
    sceClibPrintf("[RA DEBUG] Logging in with hardcoded credentials\n");
    login_retroachievements_user(username, token);
  } else {
    sceClibPrintf("[RA DEBUG] No stored credentials found, skipping login\n");
    rc_client_destroy(g_client);
    g_client = NULL;
    return 1;
  }

  // scan_and_cache_titleids();
  start_titleid_polling();

  return 0;
}

void destroy_retroachievements_client(void) {
  rc_client_destroy(g_client);
  g_client = NULL;
}