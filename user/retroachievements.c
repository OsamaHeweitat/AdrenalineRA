#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h> // Include pthread for threading
#include "retroachievements.h"
#include <rc_client.h>

#include <psp2/kernel/clib.h> 
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>

rc_client_t* g_client = NULL;

// Structure to hold async callback data
typedef struct {
    rc_client_server_callback_t callback;
    void* callback_data;
} async_callback_data;

int show_message(const char* message, ...)
{
  sceClibPrintf(message);

  return 0;
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

    // Read response
    char* response_buffer = malloc(8192); // Allocate reasonable buffer for response
    if (!response_buffer) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to allocate memory for response");
        return;
    }

    int read_size = sceHttpReadData(req, response_buffer, 8192);
    if (read_size >= 0) {
        callback(status_code, response_buffer, read_size, userdata, NULL);
    } else {
        callback(0, NULL, 0, userdata, "Failed to read HTTP response");
    }

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
    // sceHttpAddRequestHeader(req, "Content-Type", content_type, SCE_HTTP_HEADER_ADD);
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

    // Read response
    char* response_buffer = malloc(8192); // Allocate reasonable buffer for response
    if (!response_buffer) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        callback(0, NULL, 0, userdata, "Failed to allocate memory for response");
        return;
    }

    int read_size = sceHttpReadData(req, response_buffer, 8192);
    if (read_size >= 0) {
        callback(status_code, response_buffer, read_size, userdata, NULL);
    } else {
        callback(0, NULL, 0, userdata, "Failed to read HTTP response");
    }

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

static void login_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  // If not successful, just report the error and bail.
  if (result != RC_OK)
  {
    show_message("Login failed: %s", error_message);
    return;
  }

  // Login was successful. Capture the token for future logins so we don't have to store the password anywhere.
  const rc_client_user_t* user = rc_client_get_user_info(client);
  // store_retroachievements_credentials(user->username, user->token);

  // Inform user of successful login
  show_message("Logged in as %s (%u points)", user->display_name, user->score);
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
  char* response_buffer = malloc(8192);
  if (!response_buffer) {
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
    return 0;
  }
  // print
  int read_size = sceHttpReadData(req, response_buffer, 8192);
  if (read_size >= 0) {
    sceClibPrintf("Response: %s\n", response_buffer);
  } else {
    sceClibPrintf("Failed to read HTTP response test\n");
  }

  initialize_retroachievements_client();
  login_retroachievements_user("driagonv", "mother1978");

  return 0;
}