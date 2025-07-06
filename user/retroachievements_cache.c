#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/devctl.h>
#include <psp2/io/stat.h>
#include <string.h>
#include <stdlib.h>

#include "main.h"
#include "menu.h"
#include "../adrenaline_compat.h"
#include <ctype.h>

#include "retroachievements_iso_parser.h"
#include "retroachievements_config.h"

typedef struct {
    char titleid[MAX_TITLEID];
    char path[MAX_PATH];
} TitleIdEntry;

static TitleIdEntry titleid_cache[MAX_CACHE_ENTRIES];
static int titleid_cache_count = 0;

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

void start_titleid_polling() {
    save_titleid_cache();
    load_titleid_cache();
    SceUID thid = sceKernelCreateThread("titleid_polling_thread", titleid_polling_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}