#ifndef RETROACHIEVEMENTS_CACHE_H
#define RETROACHIEVEMENTS_CACHE_H

#include "retroachievements_config.h"
#include <psp2/types.h>

// Cache management functions
void save_titleid_cache(void);
void load_titleid_cache(void);
const char* lookup_path_by_titleid(const char* titleid);

// Title ID polling
void start_titleid_polling(void);
int titleid_polling_thread(SceSize args, void* argp);

#endif // RETROACHIEVEMENTS_CACHE_H