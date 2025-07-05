#ifndef RETROACHIEVEMENTS_NETWORK_H
#define RETROACHIEVEMENTS_NETWORK_H

#include "retroachievements_config.h"
#include <psp2/types.h>
#include <vita2d.h>

// Network initialization
void net_init(void);
void net_term(void);

// HTTP initialization
void http_init(void);
void http_term(void);

// SSL initialization
void ssl_init(void);
void ssl_term(void);

// HTTP functions
void async_http_get(const char* url, const char* user_agent, 
    void (*callback)(int, const char*, size_t, void*, const char*), void* userdata);

void async_http_post(const char* url, const char* post_data, const char* user_agent,
    void (*callback)(int, const char*, size_t, void*, const char*), void* userdata, const char* content_type);

// Image downloading
vita2d_texture* download_image_texture(const char* image_url, const char* cache_filename);
vita2d_texture* download_avatar_texture(const char* avatar_url);
vita2d_texture* download_achievement_badge(const char* badge_url, const char* badge_name, int is_locked);
vita2d_texture* download_game_icon(const char* icon_url, const char* game_name);

#endif // RETROACHIEVEMENTS_NETWORK_H