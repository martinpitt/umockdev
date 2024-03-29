#pragma once

/* variants of glibc functions that abort() on ENOMEM */
void *mallocx (size_t size);
void *callocx (size_t nmemb, size_t size);
char *strdupx (const char *s);
