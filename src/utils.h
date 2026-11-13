#pragma once

/* capacity of the preload's fd maps, for tracked device and socket fds */
#define FD_MAP_MAX 50

/* variants of glibc functions that exit on ENOMEM */
void *mallocx (size_t size);
void *callocx (size_t nmemb, size_t size);
char *strdupx (const char *s);
