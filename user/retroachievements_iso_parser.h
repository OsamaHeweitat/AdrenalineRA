#ifndef ISO9660_PARSER_H
#define ISO9660_PARSER_H

#include <psp2/types.h>

// ISO9660 parsing functions
void* cd_open_track(const char* path, uint32_t track);
size_t cd_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes);
uint32_t cd_first_track_sector(void* track_handle);
void cd_close_track(void* track_handle);
uint32_t cd_find_file_sector(void* track_handle, const char* path, uint32_t* size);

// SFO extraction
int extract_titleid_from_iso(const char* iso_path, char* titleid, size_t titleid_size);

#endif