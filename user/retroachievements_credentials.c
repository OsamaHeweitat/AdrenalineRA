#include "retroachievements_config.h"
#include "menu.h"
#include <psp2/types.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/devctl.h>
extern char g_ra_username[128];
extern char g_ra_password[128];

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

void update_credentials_from_menu(void) {
    if (g_ra_username[0] && g_ra_password[0]) {
        // Save to credentials file
        store_retroachievements_credentials_from_menu(g_ra_username, g_ra_password);
        sceClibPrintf("[RA DEBUG] Credentials updated from menu: %s\n", g_ra_username);
        // Optionally clear password after saving for security
        // memset(g_ra_password, 0, sizeof(g_ra_password));
    }
}