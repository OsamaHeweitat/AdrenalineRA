#include "retroachievements_config.h"

#include <psp2/kernel/clib.h> 
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>
#include <psp2/types.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <vita2d.h>

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

// Vita HTTP implementation for GET requests
void async_http_get(const char* url, const char* user_agent, 
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
    // sceClibPrintf("[RA DEBUG] HTTP response (%d bytes): %s\n", (int)total_read, response_buffer);
    callback(status_code, response_buffer, total_read, userdata, NULL);

    free(response_buffer);
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
}

// Vita HTTP implementation for POST requests
void async_http_post(const char* url, const char* post_data, const char* user_agent,
    void (*callback)(int, const char*, size_t, void*, const char*), void* userdata, const char* content_type)
{
    int tpl = sceHttpCreateTemplate(user_agent, 1, 1);
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_POST, url, strlen(post_data));
    sceHttpAddRequestHeader(req, "Content-Type", content_type, SCE_HTTP_HEADER_ADD);
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
    // sceClibPrintf("[RA DEBUG] HTTP response (%d bytes): %s\n", (int)total_read, response_buffer);
    callback(status_code, response_buffer, total_read, userdata, NULL);

    free(response_buffer);
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
}

vita2d_texture* download_image_texture(const char* image_url, const char* cache_filename) {
    if (!image_url || !image_url[0]) {
        sceClibPrintf("[RA DEBUG] No image URL provided\n");
        return NULL;
    }
    
    // sceClibPrintf("[RA DEBUG] Downloading image from: %s\n", image_url);
    
    // Determine file path - use cache if provided, otherwise temp
    char file_path[256];
    if (cache_filename && cache_filename[0]) {
        snprintf(file_path, sizeof(file_path), "%s/%s", CACHE_DIR, cache_filename);
        sceIoMkdir(CACHE_DIR, 0777);
    } else {
        snprintf(file_path, sizeof(file_path), "%s/ra_temp_image.jpg", CACHE_DIR);
    }
    
    // Check if cached version exists first
    if (cache_filename && cache_filename[0]) {
        SceIoStat stat;
        if (sceIoGetstat(file_path, &stat) >= 0) {
            sceClibPrintf("[RA DEBUG] Using cached image: %s\n", file_path);
            vita2d_texture* texture = vita2d_load_JPEG_file(file_path);
            if (!texture) {
                texture = vita2d_load_PNG_file(file_path);
            }
            if (texture) {
                return texture;
            }
        }
    }
    
    int tpl = sceHttpCreateTemplate("Adrenaline/1.0-debug (PSVita)", 1, 1);
    if (tpl < 0) {
        sceClibPrintf("[RA DEBUG] Failed to create HTTP template for image\n");
        return NULL;
    }
    
    int conn = sceHttpCreateConnectionWithURL(tpl, image_url, 0);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        sceClibPrintf("[RA DEBUG] Failed to create HTTP connection for image\n");
        return NULL;
    }
    
    int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, image_url, 0);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        sceClibPrintf("[RA DEBUG] Failed to create HTTP request for image\n");
        return NULL;
    }
    
    int res = sceHttpSendRequest(req, NULL, 0);
    if (res < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        sceClibPrintf("[RA DEBUG] Failed to send HTTP request for image\n");
        return NULL;
    }
    
    int status_code = 0;
    sceHttpGetStatusCode(req, &status_code);
    if (status_code != 200) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        sceClibPrintf("[RA DEBUG] Image download failed with status: %d\n", status_code);
        return NULL;
    }
    
    SceUID fd = sceIoOpen(file_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        sceClibPrintf("[RA DEBUG] Failed to open file for image: %s\n", file_path);
        return NULL;
    }
    
    char buffer[4096];
    int total_read = 0;
    int read_size;
    do {
        read_size = sceHttpReadData(req, buffer, sizeof(buffer));
        if (read_size > 0) {
            int written = sceIoWrite(fd, buffer, read_size);
            if (written < 0) {
                sceClibPrintf("[RA DEBUG] Failed to write image data\n");
                break;
            }
            total_read += written;
        }
    } while (read_size > 0);
    
    sceIoClose(fd);
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
    
    if (total_read == 0) {
        sceClibPrintf("[RA DEBUG] No image data downloaded\n");
        return NULL;
    }
    
    // sceClibPrintf("[RA DEBUG] Downloaded %d bytes of image data to %s\n", total_read, file_path);
    
    // Load the image as a vita2d texture
    vita2d_texture* texture = vita2d_load_JPEG_file(file_path);
    if (!texture) {
        // sceClibPrintf("[RA DEBUG] Failed to load image as JPEG texture\n");
        // Try PNG as fallback
        texture = vita2d_load_PNG_file(file_path);
        if (!texture) {
            sceClibPrintf("[RA DEBUG] Failed to load image as JPEG or PNG texture\n");
        }
    }
    
    // Clean up temp file if not caching
    if (!cache_filename || !cache_filename[0]) {
        sceIoRemove(file_path);
    }
    
    if (texture) {
        sceClibPrintf("[RA DEBUG] Successfully loaded image texture\n");
    }
    
    return texture;
}

vita2d_texture* download_game_icon(const char* icon_url, const char* game_name) {
    if (!icon_url || !icon_url[0]) return NULL;
    
    char cache_filename[128];
    snprintf(cache_filename, sizeof(cache_filename), "game_%s.png", game_name);
    
    return download_image_texture(icon_url, cache_filename);
}

// Convenience functions for specific image types
vita2d_texture* download_avatar_texture(const char* avatar_url) {
    if (!avatar_url || !avatar_url[0]) return NULL;
    
    // Generate cache filename from URL
    char cache_filename[128];
    const char* last_slash = strrchr(avatar_url, '/');
    if (last_slash) {
        snprintf(cache_filename, sizeof(cache_filename), "avatar_%s", last_slash + 1);
    } else {
        snprintf(cache_filename, sizeof(cache_filename), "avatar_%s.jpg", "default");
    }
    
    return download_image_texture(avatar_url, cache_filename);
}

vita2d_texture* download_achievement_badge(const char* badge_url, const char* badge_name, int is_locked) {
    if (!badge_url || !badge_url[0]) return NULL;
    
    // Generate cache filename
    char cache_filename[128];
    if (is_locked) {
        snprintf(cache_filename, sizeof(cache_filename), "badge_%s_locked.png", badge_name);
    } else {
        snprintf(cache_filename, sizeof(cache_filename), "badge_%s.png", badge_name);
    }
    
    return download_image_texture(badge_url, cache_filename);
}