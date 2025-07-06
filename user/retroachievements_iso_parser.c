#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include "virtualsfo.h"
#define ISO_SECTOR_SIZE 2048

// ISO9660 parsing functions (adapted from hash_disc.c)
void* cd_open_track(const char* path, uint32_t track) {
    // For ISO files, we just return the file path as the track handle
    return (void*)path;
}

size_t cd_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes) {
    const char* path = (const char*)track_handle;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    
    // Seek to sector (each sector is 2048 bytes)
    int64_t offset = (int64_t)sector * 2048;
    sceIoLseek(fd, offset, SCE_SEEK_SET);
    
    size_t bytes_read = sceIoRead(fd, buffer, requested_bytes);
    sceIoClose(fd);
    return bytes_read;
}

uint32_t cd_first_track_sector(void* track_handle) {
    // For ISO files, first track starts at sector 0
    return 0;
}

void cd_close_track(void* track_handle) {
    // Nothing to close for ISO files
}

uint32_t cd_find_file_sector(void* track_handle, const char* path, uint32_t* size) {
    uint8_t buffer[2048], *tmp;
    int sector;
    uint32_t num_sectors = 0;
    size_t filename_length;
    const char* slash;

    if (!track_handle)
        return 0;

    /* we start at the root. don't need to explicitly find it */
    if (*path == '\\')
        ++path;

    filename_length = strlen(path);
    slash = strrchr(path, '\\');
    if (slash) {
        /* find the directory record for the first part of the path */
        memcpy(buffer, path, slash - path);
        buffer[slash - path] = '\0';

        sector = cd_find_file_sector(track_handle, (const char *)buffer, NULL);
        if (!sector)
            return 0;

        ++slash;
        filename_length -= (slash - path);
        path = slash;
    }
    else {
        uint32_t logical_block_size;

        /* find the cd information */
        if (!cd_read_sector(track_handle, cd_first_track_sector(track_handle) + 16, buffer, 256))
            return 0;

        /* the directory_record starts at 156, the sector containing the table of contents is 2 bytes into that. */
        sector = buffer[156 + 2] | (buffer[156 + 3] << 8) | (buffer[156 + 4] << 16);

        /* if the table of contents spans more than one sector, it's length of section will exceed it's logical block size */
        logical_block_size = (buffer[128] | (buffer[128 + 1] << 8)); /* logical block size */
        if (logical_block_size == 0) {
            num_sectors = 1;
        } else {
            num_sectors = (buffer[156 + 10] | (buffer[156 + 11] << 8) | (buffer[156 + 12] << 16) | (buffer[156 + 13] << 24)); /* length of section */
            num_sectors /= logical_block_size;
        }
    }

    /* fetch and process the directory record */
    if (!cd_read_sector(track_handle, sector, buffer, sizeof(buffer)))
        return 0;

    tmp = buffer;
    do {
        if (tmp >= buffer + sizeof(buffer) || !*tmp) {
            /* end of this path table block. if the path table spans multiple sectors, keep scanning */
            if (num_sectors > 1) {
                --num_sectors;
                if (cd_read_sector(track_handle, ++sector, buffer, sizeof(buffer))) {
                    tmp = buffer;
                    continue;
                }
            }
            break;
        }

        /* filename is 33 bytes into the record and the format is "FILENAME;version" or "DIRECTORY" */
        if ((tmp[32] == filename_length || tmp[33 + filename_length] == ';') &&
            strncasecmp((const char*)(tmp + 33), path, filename_length) == 0) {
            sector = tmp[2] | (tmp[3] << 8) | (tmp[4] << 16);

            if (size)
                *size = tmp[10] | (tmp[11] << 8) | (tmp[12] << 16) | (tmp[13] << 24);

            return sector;
        }

        /* the first byte of the record is the length of the record */
        tmp += *tmp;
    } while (1);

    return 0;
}

// Extract title ID from PARAM.SFO in ISO
int extract_titleid_from_iso(const char* iso_path, char* titleid, size_t titleid_size) {
    void* track_handle = cd_open_track(iso_path, 1);
    if (!track_handle) {
        sceClibPrintf("[RA DEBUG] Could not open ISO track: %s\n", iso_path);
        return -1;
    }

    uint32_t sfo_size;
    uint32_t sector = cd_find_file_sector(track_handle, "PSP_GAME\\PARAM.SFO", &sfo_size);
    if (!sector) {
        sceClibPrintf("[RA DEBUG] Could not find PARAM.SFO in ISO: %s\n", iso_path);
        cd_close_track(track_handle);
        return -1;
    }

    // Read PARAM.SFO into memory
    uint8_t* sfo_data = malloc(sfo_size);
    if (!sfo_data) {
        cd_close_track(track_handle);
        return -1;
    }

    size_t total_read = 0;
    uint32_t current_sector = sector;
    while (total_read < sfo_size) {
        size_t bytes_to_read = (sfo_size - total_read > 2048) ? 2048 : sfo_size - total_read;
        size_t read = cd_read_sector(track_handle, current_sector, sfo_data + total_read, bytes_to_read);
        if (read == 0) break;
        total_read += read;
        current_sector++;
    }

    cd_close_track(track_handle);

    if (total_read < sfo_size) {
        free(sfo_data);
        return -1;
    }

    // Parse SFO to extract title ID
    sfo_parse(sfo_data);
    char* disc_id = sfo_get_string_param("DISC_ID");
    if (disc_id) {
        strncpy(titleid, disc_id, titleid_size - 1);
        titleid[titleid_size - 1] = '\0';
        free(sfo_data);
        return 0;
    }

    free(sfo_data);
    return -1;
}