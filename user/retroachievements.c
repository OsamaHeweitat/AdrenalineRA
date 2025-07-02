#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h> // Include pthread for threading
#include "retroachievements.h"
#include <rc_client.h>
#include <rc_consoles.h>

#include <psp2/kernel/clib.h> 
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>
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

rc_client_t* g_client = NULL;

// Structure to hold async callback data
typedef struct {
    rc_client_server_callback_t callback;
    void* callback_data;
} async_callback_data;

// --- Notification overlay state ---
static char g_notification_msg[128] = {0};
static SceUInt64 g_notification_until = 0;

// Call this from any thread/context to trigger a notification
void trigger_vita2d_notification(const char* message, unsigned duration_us) {
    strncpy(g_notification_msg, message, sizeof(g_notification_msg)-1);
    g_notification_msg[sizeof(g_notification_msg)-1] = '\0';
    g_notification_until = sceKernelGetProcessTimeWide() + duration_us;
}

void draw_vita2d_notification(void) {
    if (g_notification_msg[0] && sceKernelGetProcessTimeWide() < g_notification_until) {
        float notif_width = 600.0f;
        float notif_height = 60.0f;
        float notif_x = (960.0f - notif_width) / 2.0f;
        float notif_y = 544.0f - notif_height - 40.0f;
        vita2d_draw_rectangle(notif_x, notif_y, notif_width, notif_height, 0xC0000000);
        float text_width = vita2d_pgf_text_width(font, 1.0f, g_notification_msg);
        vita2d_pgf_draw_text(font, notif_x + (notif_width - text_width) / 2.0f, notif_y + 40.0f, 0xFFFFFFFF, 1.0f, g_notification_msg);
    } else if (g_notification_msg[0]) {
        g_notification_msg[0] = 0;
    }
}

int show_message(const char* message, ...)
{
  sceClibPrintf(message);

  return 0;
}

// Store credentials to a file for persistence
void store_retroachievements_credentials(const char* username, const char* token) {
    SceUID fd = sceIoOpen("ux0:data/ra_credentials.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, username, strlen(username));
        sceIoWrite(fd, ":", 1);
        sceIoWrite(fd, token, strlen(token));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
        sceClibPrintf("[RA DEBUG] Credentials saved to ux0:data/ra_credentials.txt\n");
    } else {
        sceClibPrintf("[RA DEBUG] Failed to open credentials file for writing\n");
    }
}

// This is the function the rc_client will use to read memory for the emulator. we don't need it yet,
// so just provide a dummy function that returns "no memory read".
static uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  // TODO: implement later
  return 0;
}

// Vita HTTP implementation for GET requests
static void async_http_get(const char* url, const char* user_agent, 
    void (*callback)(int, const char*, size_t, void*, const char*), void* userdata)
{
    int tpl = sceHttpCreateTemplate(user_agent, 1, 1);
    if (tpl < 0) {
        callback(0, NULL, 0, userdata, "Failed to create HTTP template");
        return;
    }

    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to create HTTP connection");
        return;
    }

    int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to create HTTP request");
        return;
    }

    int res = sceHttpSendRequest(req, NULL, 0);
    if (res < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to send HTTP request");
        return;
    }

    // Get status code
    int status_code = 0;
    sceHttpGetStatusCode(req, &status_code);

    // Read response with dynamic buffer
    size_t buffer_size = 32768; // Start with 32KB
    char* response_buffer = malloc(buffer_size);
    if (!response_buffer) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to allocate memory for response");
        return;
    }

    size_t total_read = 0;
    int read_size;
    do {
        read_size = sceHttpReadData(req, response_buffer + total_read, buffer_size - total_read - 1);
        if (read_size > 0) {
            total_read += read_size;
            // If we're running out of space, double the buffer
            if (total_read >= buffer_size - 1024) {
                size_t new_size = buffer_size * 2;
                char* new_buffer = realloc(response_buffer, new_size);
                if (!new_buffer) {
                    free(response_buffer);
                    sceHttpDeleteRequest(req);
                    sceHttpDeleteConnection(conn);
                    sceHttpDeleteTemplate(tpl);
                    callback(0, NULL, 0, userdata, "Failed to reallocate memory for response");
                    return;
                }
                response_buffer = new_buffer;
                buffer_size = new_size;
            }
        }
    } while (read_size > 0);

    if (read_size < 0) {
        free(response_buffer);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to read HTTP response");
        return;
    }

    response_buffer[total_read] = '\0'; // Null-terminate the response
    sceClibPrintf("[RA DEBUG] HTTP response (%d bytes): %s\n", (int)total_read, response_buffer);
    callback(status_code, response_buffer, total_read, userdata, NULL);

    free(response_buffer);
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
}

// Vita HTTP implementation for POST requests
static void async_http_post(const char* url, const char* post_data, const char* user_agent,
    void (*callback)(int, const char*, size_t, void*, const char*), void* userdata, const char* content_type)
{
    int tpl = sceHttpCreateTemplate(user_agent, 1, 1);
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_POST, url, strlen(post_data));
    sceHttpAddRequestHeader(req, "Content-Type", content_type, SCE_HTTP_HEADER_ADD);
    sceClibPrintf("POST URL: %s\n", url);
    sceClibPrintf("POST Data: %s\n", post_data);
    sceClibPrintf("User-Agent: %s\n", user_agent);
    sceClibPrintf("Content-Type: %s\n", content_type);
    int res = sceHttpSendRequest(req, post_data, strlen(post_data));
    if (res < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to send HTTP request");
        return;
    }

    // Get status code
    int status_code = 0;
    sceHttpGetStatusCode(req, &status_code);

    // Read response with dynamic buffer
    size_t buffer_size = 32768; // Start with 32KB
    char* response_buffer = malloc(buffer_size);
    if (!response_buffer) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to allocate memory for response");
        return;
    }

    size_t total_read = 0;
    int read_size;
    do {
        read_size = sceHttpReadData(req, response_buffer + total_read, buffer_size - total_read - 1);
        if (read_size > 0) {
            total_read += read_size;
            // If we're running out of space, double the buffer
            if (total_read >= buffer_size - 1024) {
                size_t new_size = buffer_size * 2;
                char* new_buffer = realloc(response_buffer, new_size);
                if (!new_buffer) {
                    free(response_buffer);
                    sceHttpDeleteRequest(req);
                    sceHttpDeleteConnection(conn);
                    sceHttpDeleteTemplate(tpl);
                    callback(0, NULL, 0, userdata, "Failed to reallocate memory for response");
                    return;
                }
                response_buffer = new_buffer;
                buffer_size = new_size;
            }
        }
    } while (read_size > 0);

    if (read_size < 0) {
        free(response_buffer);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to read HTTP response");
        return;
    }

    response_buffer[total_read] = '\0'; // Null-terminate the response
    sceClibPrintf("[RA DEBUG] HTTP response (%d bytes): %s\n", (int)total_read, response_buffer);
    callback(status_code, response_buffer, total_read, userdata, NULL);

    free(response_buffer);
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
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

  sceClibPrintf("[RA DEBUG] http_callback: status=%d\n", server_response.http_status_code);
  sceClibPrintf("[RA DEBUG] http_callback: body_length=%d\n", (int)server_response.body_length);
  sceClibPrintf("[RA DEBUG] http_callback: body=%s\n", server_response.body);

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

void initialize_retroachievements_client(void)
{
  // Create the client instance (using a global variable simplifies this example)
  g_client = rc_client_create(read_memory, server_call);

  // Provide a logging function to simplify debugging
  rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_VERBOSE, log_message);

  // Disable hardcore - if we goof something up in the implementation, we don't want our
  // account disabled for cheating.
  rc_client_set_hardcore_enabled(g_client, 0);
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

extern vita2d_pgf *font; // Use the global font from menu.c

// void show_vita2d_notification(const char* message) {
//     sceClibPrintf("show_vita2d_notification: %s\n", message);
//     // Draw overlay for 2 seconds
//     SceUInt64 start = sceKernelGetProcessTimeWide();
//     SceUInt64 now = start;
//     while (now - start < 2000000) { // 2,000,000 microseconds = 2 seconds
//         vita2d_start_drawing();
//         // Optionally: vita2d_clear_screen(); // Don't clear, just overlay
//         // Draw a semi-transparent black box at the bottom of the screen
//         float notif_width = 600.0f;
//         float notif_height = 60.0f;
//         float notif_x = (960.0f - notif_width) / 2.0f;
//         float notif_y = 544.0f - notif_height - 40.0f;
//         vita2d_draw_rectangle(notif_x, notif_y, notif_width, notif_height, 0xC0000000);
//         // Draw the message centered
//         float text_width = vita2d_pgf_text_width(font, 1.0f, message);
//         vita2d_pgf_draw_text(font, notif_x + (notif_width - text_width) / 2.0f, notif_y + 40.0f, 0xFFFFFFFF, 1.0f, message);
//         vita2d_end_drawing();
//         vita2d_swap_buffers();
//         sceKernelDelayThread(16 * 1000); // ~60fps
//         now = sceKernelGetProcessTimeWide();
//     }
// }

static void login_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  // If not successful, just report the error and bail.
  if (result != RC_OK)
  {
    sceClibPrintf("Login failed: %s\n", error_message);
    return;
  }

  // Login was successful. Capture the token for future logins so we don't have to store the password anywhere.
  const rc_client_user_t* user = rc_client_get_user_info(client);
  
  sceClibPrintf("[RA DEBUG] login_callback: user pointer=%p\n", user);
  if (user) {
    sceClibPrintf("[RA DEBUG] login_callback: username=%s\n", user->username);
    sceClibPrintf("[RA DEBUG] login_callback: display_name=%s\n", user->display_name);
    sceClibPrintf("[RA DEBUG] login_callback: score=%u\n", user->score);
  } else {
    sceClibPrintf("[RA DEBUG] login_callback: user is NULL\n");
  }
  
  store_retroachievements_credentials(user->username, user->token);

  // Inform user of successful login
  char login_msg[128];
  snprintf(login_msg, sizeof(login_msg), "Logged in as %s (%u points)", user->display_name, user->score);
  sceClibPrintf("%s", login_msg);

  // Trigger notification (2 seconds)
  trigger_vita2d_notification(login_msg, 2000000);
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

void net_init() {
  // sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

  static char memory[16 * 1024];
	SceNetInitParam param;
	param.memory = memory;
	param.size = sizeof(memory);
	param.flags = 0;

	int res = sceNetInit(&param);
  if (res < 0) {
    sceClibPrintf("sceNetInit failed (0x%X)\n", res);
  }

	res = sceNetCtlInit();
  if (res < 0) {
    sceClibPrintf("sceNetCtlInit failed (0x%X)\n", res);
  }
}

void net_term() {
  sceNetCtlTerm();

  sceNetTerm();

  sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

void http_init() {
  // sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);

  // sceHttpInit(1 * 1024 * 1024);
  int res = sceHttpInit(1 * 1024 * 1024);
  if (res < 0) {
    sceClibPrintf("sceHttpInit failed (0x%X)\n", res);
  } else {
    sceClibPrintf("sceHttpInit success (0x%X)\n", res);
  }
}

void http_term() {
  sceHttpTerm();

  sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
}

void ssl_init() {
  // sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);

  int res = sceSslInit(1 * 1024 * 1024);
  if (res < 0) {
    sceClibPrintf("sceSslInit failed (0x%X)\n", res);
  } else {
    sceClibPrintf("sceSslInit success (0x%X)\n", res);
  }
}

void ssl_term() {
  sceSslEnd();

  sceSysmoduleUnloadModule(SCE_SYSMODULE_SSL);
}

// Load credentials from file
int load_retroachievements_credentials(char* username, size_t username_size, char* token, size_t token_size) {
    SceUID fd = sceIoOpen("ux0:data/ra_credentials.txt", SCE_O_RDONLY, 0);
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
    if (!sep) {
        sceClibPrintf("[RA DEBUG] Credentials file format invalid\n");
        return -1;
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
    return 0;
}

// static void show_game_placard(void)
// {
//   char message[128], url[128];
//   async_image_data* image_data = NULL;
//   const rc_client_game_t* game = rc_client_get_game_info(g_client);
//   rc_client_user_game_summary_t summary;
//   rc_client_get_user_game_summary(g_client, &summary);

//   // Construct a message indicating the number of achievements unlocked by the user.
//   if (summary.num_core_achievements == 0)
//   {
//     snprintf(message, sizeof(message), "This game has no achievements.");
//   }
//   else if (summary.num_unsupported_achievements)
//   {
//     snprintf(message, sizeof(message), "You have %u of %u achievements unlocked (%d unsupported).",
//         summary.num_unlocked_achievements, summary.num_core_achievements,
//         summary.num_unsupported_achievements);
//   }
//   else
//   {
//     snprintf(message, sizeof(message), "You have %u of %u achievements unlocked.",
//         summary.num_unlocked_achievements, summary.num_core_achievements);
//   }

//   // The emulator is responsible for managing images. This uses rc_client to build
//   // the URL where the image should be downloaded from.
//   if (rc_client_game_get_image_url(game, url, sizeof(url)) == RC_OK)
//   {
//     // Generate a local filename to store the downloaded image.
//     char game_badge[64];
//     snprintf(game_badge, sizeof(game_badge), "game_%s.png", game->badge_name);

//     // This function will download and cache the game image. It is up to the emulator
//     // to implement this logic. Similarly, the emulator has to use image_data to
//     // display the game badge in the placard, or a placeholder until the image is
//     // downloaded. None of that logic is provided in this example.
//     image_data = download_and_cache_image(game_badge, url);
//   } 

//   show_popup_message(image_data, game->title, message);
// }

static void load_game_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  if (result != RC_OK)
  {
    show_message("RetroAchievements game load failed: %s", error_message);
    return;
  }

  // announce that the game is ready. we'll cover this in the next section.
  // show_game_placard();
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
  sceClibPrintf("[RA DEBUG] Calling rc_client_begin_identify_and_load_game (file) with path: %s\n", path);
  rc_client_begin_identify_and_load_game(g_client, RC_CONSOLE_PSP, path, NULL, 0, load_game_callback, NULL);
}

#define ISO_DIR "ux0:pspemu/ISO"
#define GAME_DIR "ux0:pspemu/PSP/GAME"
#define CACHE_FILE "ux0:data/ra_titleid_cache.txt"

#define MAX_TITLEID 64
#define MAX_PATH 256
#define MAX_CACHE_ENTRIES 1024

typedef struct {
    char titleid[MAX_TITLEID];
    char path[MAX_PATH];
} TitleIdEntry;

static TitleIdEntry titleid_cache[MAX_CACHE_ENTRIES];
static int titleid_cache_count = 0;

#define ISO_SECTOR_SIZE 2048

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
            strncpy(titleid_cache[titleid_cache_count].path, sep+1, MAX_PATH-1);
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
    while (1) {
        // Read adrenaline->titleid from shared memory
        SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
        char titleid[MAX_TITLEID] = {0};
        strncpy(titleid, adrenaline->titleid, MAX_TITLEID-1);
        titleid[MAX_TITLEID-1] = '\0';
        if (titleid[0] != '\0' && strcmp(titleid, last_titleid) != 0) {
            sceClibPrintf("[RA DEBUG] New titleid detected: %s\n", titleid);
            const char* path = lookup_path_by_titleid(titleid);
            if (path) {
                sceClibPrintf("[RA DEBUG] Path for titleid %s: %s\n", titleid, path);
                load_game_from_file(path);
            } else {
                sceClibPrintf("[RA DEBUG] Path for titleid %s not found in cache\n", titleid);
            }
            strncpy(last_titleid, titleid, MAX_TITLEID-1);
        }
        sceKernelDelayThread(1 * 1000 * 1000); // 1 second
    }
    return 0;
}

// Call this at startup
void start_titleid_polling() {
    load_titleid_cache();
    SceUID thid = sceKernelCreateThread("titleid_polling_thread", titleid_polling_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
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

  int tpl = sceHttpCreateTemplate("Adrenaline/1.0-debug (PSVita)", 1, 1);
  int conn = sceHttpCreateConnectionWithURL(tpl, "https://httpbin.org/post", 0);
  int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_POST, "https://httpbin.org/post", 0);
  sceHttpAddRequestHeader(req, "Content-Type", "application/json", SCE_HTTP_HEADER_ADD);
  int res = sceHttpSendRequest(req, "{\"key\":\"value\"}", 13);
  if (res < 0) {
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
    sceClibPrintf("Failed to send HTTP request test\n");
    return 0;
  }

  // read and print response
  char* response_buffer = malloc(8192 + 1);
  if (!response_buffer) {
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
    return 0;
  }
  // print
  int read_size = sceHttpReadData(req, response_buffer, 8192);
  if (read_size >= 0) {
    response_buffer[read_size] = '\0'; // Null-terminate the response
    sceClibPrintf("Response: %s\n", response_buffer);
  } else {
    sceClibPrintf("Failed to read HTTP response test\n");
  }

  initialize_retroachievements_client();

  char username[128];
  char token[128];
  if (load_retroachievements_credentials(username, sizeof(username), token, sizeof(token)) == 0) {
    sceClibPrintf("[RA DEBUG] Logging in with stored credentials\n");
    login_remembered_retroachievements_user(username, token);
  } else {
    sceClibPrintf("[RA DEBUG] Logging in with hardcoded credentials\n");
    login_retroachievements_user("driagonv", "REMOVED");
  }

  // scan_and_cache_titleids();
  start_titleid_polling();

  return 0;
}