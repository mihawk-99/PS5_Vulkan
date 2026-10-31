/* Read the title metadata for timing receipts; failure is reported separately. */
#ifndef PS5VK_PRESENT_METADATA_H
#define PS5VK_PRESENT_METADATA_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int
present_read_attribute3(uint32_t *value)
{
   FILE *file = fopen("/app0/sce_sys/param.json", "rb");
   if (!file)
      return 0;
   char text[16384];
   size_t length = fread(text, 1, sizeof(text) - 1, file);
   int failed = ferror(file);
   fclose(file);
   if (failed || length == sizeof(text) - 1)
      return 0;
   text[length] = '\0';
   const char *at = strstr(text, "\"attribute3\"");
   if (!at || !(at = strchr(at, ':')))
      return 0;
   char *end;
   errno = 0;
   unsigned long parsed = strtoul(at + 1, &end, 0);
   if (errno || end == at + 1 || parsed > UINT32_MAX)
      return 0;
   *value = (uint32_t)parsed;
   return 1;
}

#endif
