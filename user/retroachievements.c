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

#include "retroachievements_network.h"

// Add extern declarations for SFO parsing functions
extern void sfo_parse(void* data);
extern char* sfo_get_string_param(char* key);

rc_client_t* g_client = NULL;

// Structure to hold async callback data
typedef struct {
    rc_client_server_callback_t callback;
    void* callback_data;
} async_callback_data;

// --- Notification overlay state ---
static char g_notification_msg[128] = {0};
static SceUInt64 g_notification_until = 0;
static vita2d_texture* g_notification_image = NULL; // Optional image for notification

// --- Achievements Menu State ---
static int g_show_achievements_menu = 0;
static int g_achievements_menu_sel = 0; // Currently selected item (for highlight)

typedef struct {
    int pending;
    char message[256];
    char image_url[256];
    char cache_name[128];
    int is_badge; // 1 for achievement badge, 0 for avatar
    int is_locked; // for badge only
} PendingNotification;

static PendingNotification g_pending_notification = {0};

char ISO_DIR[256];
char GAME_DIR[256];
char CACHE_FILE[256];
char CACHE_DIR[256];
char CREDENTIALS_FILE[256];

#define MAX_TITLEID 64
#define MAX_PATH 256
#define MAX_CACHE_ENTRIES 1024

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

int show_message(const char* message, ...)
{
  sceClibPrintf(message);

  return 0;
}

// Store credentials to a file for persistence
void store_retroachievements_credentials(const char* username, const char* token) {
    SceUID fd = sceIoOpen(CREDENTIALS_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); 
    if (fd >= 0) {
        sceIoWrite(fd, username, strlen(username));
        sceIoWrite(fd, ":", 1);
        sceIoWrite(fd, token, strlen(token));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
        sceClibPrintf("[RA DEBUG] Credentials saved to %s\n", CREDENTIALS_FILE);
    } else {
        sceClibPrintf("[RA DEBUG] Failed to open credentials file for writing\n");
    }
}

void store_retroachievements_credentials_from_menu(const char* username, const char* token) {
    SceUID fd = sceIoOpen(CREDENTIALS_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); 
    if (fd >= 0) {
        sceIoWrite(fd, username, strlen(username));
        sceIoWrite(fd, ";", 1);
        sceIoWrite(fd, token, strlen(token));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
        sceClibPrintf("[RA DEBUG] Credentials saved to %s\n", CREDENTIALS_FILE);
    } else {
        sceClibPrintf("[RA DEBUG] Failed to open credentials file for writing\n");
    }
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
  sceClibPrintf("server_call: url=%s\n", request->url);

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

// Load credentials from file
int load_retroachievements_credentials(char* username, size_t username_size, char* token, size_t token_size) {
    int ret = -1;
    SceUID fd = sceIoOpen(CREDENTIALS_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) {
        sceClibPrintf("[RA DEBUG] Could not open credentials file for reading\n");
        return -1;
    }
    char buf[256];
    int read = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (read <= 0) {
        sceClibPrintf("[RA DEBUG] Credentials file is empty or unreadable\n");
        return -1;
    }
    buf[read] = '\0';
    // Format: username:token\n
    char* sep = strchr(buf, ':');
    ret = 0;
    sceClibPrintf("[RA DEBUG] Credentials file format colon trying\n");
    if (!sep) {
        sceClibPrintf("[RA DEBUG] Credentials file format semi-colon trying\n");
        // sceClibPrintf("[RA DEBUG] Credentials file format invalid\n");
        // return -1;
        sep = strchr(buf, ';');
        if (!sep) {
            sceClibPrintf("[RA DEBUG] Credentials file format invalid\n");
            ret = -1;
            return ret;
        }
        ret = 1;
    }
    size_t ulen = sep - buf;
    if (ulen >= username_size) ulen = username_size - 1;
    strncpy(username, buf, ulen);
    username[ulen] = '\0';
    char* tstart = sep + 1;
    char* tend = strchr(tstart, '\n');
    size_t tlen = tend ? (size_t)(tend - tstart) : strlen(tstart);
    if (tlen >= token_size) tlen = token_size - 1;
    strncpy(token, tstart, tlen);
    token[tlen] = '\0';
    sceClibPrintf("[RA DEBUG] Loaded credentials: username=%s, token=%s\n", username, token);
    return ret;
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

typedef struct {
    char titleid[MAX_TITLEID];
    char path[MAX_PATH];
} TitleIdEntry;

static TitleIdEntry titleid_cache[MAX_CACHE_ENTRIES];
static int titleid_cache_count = 0;

#define ISO_SECTOR_SIZE 2048

// ISO9660 parsing functions (adapted from hash_disc.c)
static void* cd_open_track(const char* path, uint32_t track) {
    // For ISO files, we just return the file path as the track handle
    return (void*)path;
}

static size_t cd_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes) {
    const char* path = (const char*)track_handle;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    
    // Seek to sector (each sector is 2048 bytes)
    int64_t offset = (int64_t)sector * 2048;
    sceIoLseek(fd, offset, SCE_SEEK_SET);
    
    size_t bytes_read = sceIoRead(fd, buffer, requested_bytes);
    sceIoClose(fd);
    return bytes_read;
}

static uint32_t cd_first_track_sector(void* track_handle) {
    // For ISO files, first track starts at sector 0
    return 0;
}

static void cd_close_track(void* track_handle) {
    // Nothing to close for ISO files
}

static uint32_t cd_find_file_sector(void* track_handle, const char* path, uint32_t* size) {
    uint8_t buffer[2048], *tmp;
    int sector;
    uint32_t num_sectors = 0;
    size_t filename_length;
    const char* slash;

    if (!track_handle)
        return 0;

    /* we start at the root. don't need to explicitly find it */
    if (*path == '\\')
        ++path;

    filename_length = strlen(path);
    slash = strrchr(path, '\\');
    if (slash) {
        /* find the directory record for the first part of the path */
        memcpy(buffer, path, slash - path);
        buffer[slash - path] = '\0';

        sector = cd_find_file_sector(track_handle, (const char *)buffer, NULL);
        if (!sector)
            return 0;

        ++slash;
        filename_length -= (slash - path);
        path = slash;
    }
    else {
        uint32_t logical_block_size;

        /* find the cd information */
        if (!cd_read_sector(track_handle, cd_first_track_sector(track_handle) + 16, buffer, 256))
            return 0;

        /* the directory_record starts at 156, the sector containing the table of contents is 2 bytes into that. */
        sector = buffer[156 + 2] | (buffer[156 + 3] << 8) | (buffer[156 + 4] << 16);

        /* if the table of contents spans more than one sector, it's length of section will exceed it's logical block size */
        logical_block_size = (buffer[128] | (buffer[128 + 1] << 8)); /* logical block size */
        if (logical_block_size == 0) {
            num_sectors = 1;
        } else {
            num_sectors = (buffer[156 + 10] | (buffer[156 + 11] << 8) | (buffer[156 + 12] << 16) | (buffer[156 + 13] << 24)); /* length of section */
            num_sectors /= logical_block_size;
        }
    }

    /* fetch and process the directory record */
    if (!cd_read_sector(track_handle, sector, buffer, sizeof(buffer)))
        return 0;

    tmp = buffer;
    do {
        if (tmp >= buffer + sizeof(buffer) || !*tmp) {
            /* end of this path table block. if the path table spans multiple sectors, keep scanning */
            if (num_sectors > 1) {
                --num_sectors;
                if (cd_read_sector(track_handle, ++sector, buffer, sizeof(buffer))) {
                    tmp = buffer;
                    continue;
                }
            }
            break;
        }

        /* filename is 33 bytes into the record and the format is "FILENAME;version" or "DIRECTORY" */
        if ((tmp[32] == filename_length || tmp[33 + filename_length] == ';') &&
            strncasecmp((const char*)(tmp + 33), path, filename_length) == 0) {
            sector = tmp[2] | (tmp[3] << 8) | (tmp[4] << 16);

            if (size)
                *size = tmp[10] | (tmp[11] << 8) | (tmp[12] << 16) | (tmp[13] << 24);

            return sector;
        }

        /* the first byte of the record is the length of the record */
        tmp += *tmp;
    } while (1);

    return 0;
}

// Extract title ID from PARAM.SFO in ISO
static int extract_titleid_from_iso(const char* iso_path, char* titleid, size_t titleid_size) {
    void* track_handle = cd_open_track(iso_path, 1);
    if (!track_handle) {
        sceClibPrintf("[RA DEBUG] Could not open ISO track: %s\n", iso_path);
        return -1;
    }

    uint32_t sfo_size;
    uint32_t sector = cd_find_file_sector(track_handle, "PSP_GAME\\PARAM.SFO", &sfo_size);
    if (!sector) {
        sceClibPrintf("[RA DEBUG] Could not find PARAM.SFO in ISO: %s\n", iso_path);
        cd_close_track(track_handle);
        return -1;
    }

    // Read PARAM.SFO into memory
    uint8_t* sfo_data = malloc(sfo_size);
    if (!sfo_data) {
        cd_close_track(track_handle);
        return -1;
    }

    size_t total_read = 0;
    uint32_t current_sector = sector;
    while (total_read < sfo_size) {
        size_t bytes_to_read = (sfo_size - total_read > 2048) ? 2048 : sfo_size - total_read;
        size_t read = cd_read_sector(track_handle, current_sector, sfo_data + total_read, bytes_to_read);
        if (read == 0) break;
        total_read += read;
        current_sector++;
    }

    cd_close_track(track_handle);

    if (total_read < sfo_size) {
        free(sfo_data);
        return -1;
    }

    // Parse SFO to extract title ID
    sfo_parse(sfo_data);
    char* disc_id = sfo_get_string_param("DISC_ID");
    if (disc_id) {
        strncpy(titleid, disc_id, titleid_size - 1);
        titleid[titleid_size - 1] = '\0';
        free(sfo_data);
        return 0;
    }

    free(sfo_data);
    return -1;
}

void save_titleid_cache() {
    sceClibPrintf("[RA DEBUG] Starting title ID cache scan...\n");
    
    // Create cache directory if it doesn't exist
    sceIoMkdir(CACHE_DIR, 0777);
    
    // Open ISO directory
    SceUID dir = sceIoDopen(ISO_DIR);
    if (dir < 0) {
        sceClibPrintf("[RA DEBUG] Could not open ISO directory: %s\n", ISO_DIR);
        return;
    }

    // Open cache file for writing
    SceUID cache_fd = sceIoOpen(CACHE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (cache_fd < 0) {
        sceClibPrintf("[RA DEBUG] Could not create cache file: %s\n", CACHE_FILE);
        sceIoDclose(dir);
        return;
    }

    SceIoDirent entry;
    int processed_count = 0;
    int success_count = 0;

    while (sceIoDread(dir, &entry) > 0) {
        // Check if it's a file with .iso or .cso extension
        if (!(entry.d_stat.st_mode & SCE_S_IFDIR)) {
            const char* name = entry.d_name;
            size_t name_len = strlen(name);
            
            if ((name_len >= 4 && strcasecmp(name + name_len - 4, ".iso") == 0) ||
                (name_len >= 4 && strcasecmp(name + name_len - 4, ".cso") == 0)) {
                
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", ISO_DIR, name);
                
                sceClibPrintf("[RA DEBUG] Processing ISO: %s\n", name);
                
                char titleid[MAX_TITLEID];
                if (extract_titleid_from_iso(full_path, titleid, sizeof(titleid)) == 0) {
                    // Write to cache file: titleid|filepath
                    char cache_line[512];
                    snprintf(cache_line, sizeof(cache_line), "%s|%s\n", titleid, full_path);
                    sceIoWrite(cache_fd, cache_line, strlen(cache_line));
                    sceClibPrintf("[RA DEBUG] Cached: %s -> %s\n", titleid, full_path);
                    success_count++;
                } else {
                    sceClibPrintf("[RA DEBUG] Failed to extract title ID from: %s\n", name);
                }
                processed_count++;
            }
        }
    }

    sceIoClose(cache_fd);
    sceIoDclose(dir);
    
    sceClibPrintf("[RA DEBUG] Title ID cache scan complete. Processed %d files, cached %d title IDs.\n", 
                  processed_count, success_count);
}

// Load cache from file
void load_titleid_cache() {
    titleid_cache_count = 0;
    SceUID fd = sceIoOpen(CACHE_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[4096];
    int read = sceIoRead(fd, buf, sizeof(buf)-1);
    sceIoClose(fd);
    if (read <= 0) return;
    buf[read] = '\0';
    char* line = strtok(buf, "\n");
    while (line && titleid_cache_count < MAX_CACHE_ENTRIES) {
        char* sep = strchr(line, '|');
        if (sep) {
            *sep = '\0';
            strncpy(titleid_cache[titleid_cache_count].titleid, line, MAX_TITLEID-1);
            titleid_cache[titleid_cache_count].titleid[MAX_TITLEID-1] = '\0';
            
            // Strip carriage return from the end of the path
            char* path_start = sep + 1;
            int path_len = strlen(path_start);
            if (path_len > 0 && path_start[path_len-1] == '\r') {
                path_start[path_len-1] = '\0';
            }
            
            strncpy(titleid_cache[titleid_cache_count].path, path_start, MAX_PATH-1);
            titleid_cache[titleid_cache_count].path[MAX_PATH-1] = '\0';
            titleid_cache_count++;
        }
        line = strtok(NULL, "\n");
    }
    sceClibPrintf("[RA DEBUG] Loaded %d titleids from cache\n", titleid_cache_count);
}

// Lookup file path by titleid
const char* lookup_path_by_titleid(const char* titleid) {
    for (int i = 0; i < titleid_cache_count; ++i) {
        if (strcmp(titleid_cache[i].titleid, titleid) == 0) {
            return titleid_cache[i].path;
        }
    }
    return NULL;
}

// Polling thread: check for new titleid from adrenaline shared memory
int titleid_polling_thread(SceSize args, void* argp) {
  sceClibPrintf("[RA DEBUG] titleid_polling_thread started\n");
    char last_titleid[MAX_TITLEID] = {0};
    int game_loaded = 0; // Track if we have a game loaded
    
    while (1) {
        // Read adrenaline->titleid from shared memory
        SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
        char titleid[MAX_TITLEID] = {0};
        strncpy(titleid, adrenaline->titleid, MAX_TITLEID-1);
        titleid[MAX_TITLEID-1] = '\0';
        
        // Check if game was closed (titleid became empty but we had a game loaded)
        if (titleid[0] == '\0' && game_loaded) {
            sceClibPrintf("[RA DEBUG] Game closed, resetting state\n");
            game_loaded = 0;
            last_titleid[0] = '\0'; // Reset last titleid
            
            if (g_client) {
                sceClibPrintf("[RA DEBUG] Unloading game from RA client\n");
                rc_client_unload_game(g_client);
            }
            
            trigger_vita2d_notification("Game closed - watching for next game", 3000000, NULL);
        }
        // Check if new game was launched
        else if (titleid[0] != '\0' && strcmp(titleid, last_titleid) != 0) {
            sceClibPrintf("[RA DEBUG] New titleid detected: %s\n", titleid);
            const char* path = lookup_path_by_titleid(titleid);
            if (path) {
                sceClibPrintf("[RA DEBUG] Path for titleid %s: %s\n", titleid, path);
                load_game_from_file(path);
                game_loaded = 1; // Mark that we have a game loaded
            } else {
                sceClibPrintf("[RA DEBUG] Path for titleid %s not found in cache\n", titleid);
                // check if game exists in folder format
                SceIoStat stat;
                char game_path[256];
                snprintf(game_path, sizeof(game_path), "%s/%s", GAME_DIR, titleid);
                if (sceIoGetstat(game_path, &stat) == 0 && (stat.st_mode & SCE_S_IFDIR)) {
                    sceClibPrintf("[RA DEBUG] Found directory for titleid %s: %s\n", titleid, game_path);
                    snprintf(game_path, sizeof(game_path), "%s/%s/EBOOT.PBP", GAME_DIR, titleid);
                    load_game_from_file(game_path);
                }
                // Even if path not found, mark as loaded to avoid repeated attempts
                game_loaded = 1;
            }
            strncpy(last_titleid, titleid, MAX_TITLEID-1);
        }
        
        sceKernelDelayThread(1 * 1000 * 1000); // 1 second
    }
    return 0;
}

// Call this at startup
void start_titleid_polling() {
    save_titleid_cache();
    load_titleid_cache();
    SceUID thid = sceKernelCreateThread("titleid_polling_thread", titleid_polling_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}

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

void drawAchievementsMenu() {
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
void ctrlAchievementsMenu() {
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
        drawAchievementsMenu();
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

static void event_handler(const rc_client_event_t* event, rc_client_t* client)
{
  switch (event->type)
  {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      achievement_triggered(event->achievement);
      break;

    default:
      printf("Unhandled event %d\n", event->type);
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

// Add externs for username/password from menu.c
extern char g_ra_username[128];
extern char g_ra_password[128];

// Helper to update credentials file if user changed them from the menu
void update_credentials_from_menu(void) {
    // Only update if both are non-empty
    if (g_ra_username[0] && g_ra_password[0]) {
        // Save to credentials file
        store_retroachievements_credentials_from_menu(g_ra_username, g_ra_password);
        sceClibPrintf("[RA DEBUG] Credentials updated from menu: %s\n", g_ra_username);
        // Optionally clear password after saving for security
        // memset(g_ra_password, 0, sizeof(g_ra_password));
    }
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