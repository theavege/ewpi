#ifndef EWPI_MAP_H
#define EWPI_MAP_H

#include <stddef.h>

#ifdef _WIN32
# include <windows.h>
#endif

typedef struct
{
    unsigned char *base;
    size_t length;
#ifdef _WIN32
    HANDLE file;
    HANDLE map;
#else
    int fd;
#endif
} Map;

int ewpi_map_new(Map *map, const char *filename);

void ewpi_map_del(Map *map);

#endif /* EWPI_MAP_H */
