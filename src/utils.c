#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

void *
callocx (size_t nmemb, size_t size)
{
  void *r = calloc (nmemb, size);
  if (r == NULL)
      err (EXIT_FAILURE, "callocx: failed to allocate memory");
  return r;
}

void *
mallocx (size_t size)
{
  void *r = malloc (size);
  if (r == NULL)
      err (EXIT_FAILURE, "mallocx: failed to allocate memory");
  return r;
}


char *
strdupx (const char *s)
{
  char *r = strdup (s);
  if (r == NULL)
      err (EXIT_FAILURE, "strdupx: failed to allocate memory");
  return r;
}
