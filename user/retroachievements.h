#ifndef RETROACHIEVEMENTS_H
#define RETROACHIEVEMENTS_H

#include <rc_client.h>

#ifdef __cplusplus
extern "C" {
#endif

void initialize_retroachievements_client(void);
void shutdown_retroachievements_client(void);
void login_retroachievements_user(const char* username, const char* password);
void login_remembered_retroachievements_user(const char* username, const char* token);
void net_init(void);
void net_term(void);
void httpInit(void);
void httpTerm(void);
int start(void);
void load_game(const uint8_t* rom, size_t rom_size);
void load_game_from_file(const char* path);
void destroy_retroachievements_client(void);
void update_credentials_from_menu(void);

// Game detection functions
void start_game_detection(void);
void stop_game_detection(void);

#ifdef __cplusplus
}
#endif

#endif // RETROACHIEVEMENTS_H
