#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

static void
abort_errno (const char *msg)
{
  perror (msg);
  abort ();
}

void *
callocx (size_t nmemb, size_t size)
{
  void *r = calloc (nmemb, size);
  if (r == NULL)
      abort_errno ("failed to allocate memory");
  return r;
}

void *
mallocx (size_t size)
{
  void *r = malloc (size);
  if (r == NULL)
      abort_errno ("failed to allocate memory");
  return r;
}


char *
strdupx (const char *s)
{
  char *r = strdup (s);
  if (r == NULL)
    abort_errno ("failed to allocate memory for strdup");
  return r;
}
