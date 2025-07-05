#ifndef RETROACHIEVEMENTS_CONFIG_H
#define RETROACHIEVEMENTS_CONFIG_H

#include <psp2/types.h>

// Configuration constants
#define MAX_TITLEID 64
#define MAX_PATH 256
#define MAX_CACHE_ENTRIES 1024
#define ISO_SECTOR_SIZE 2048
#define MAX_MENU_ITEMS 128
#define MAX_MENU_TEXT 128

// File paths (extern declarations)
extern char ISO_DIR[256];
extern char GAME_DIR[256];
extern char CACHE_FILE[256];
extern char CACHE_DIR[256];
extern char CREDENTIALS_FILE[256];

#endif // RETROACHIEVEMENTS_CONFIG_H