#include <stdio.h>

#include "esp_log.h"


void esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...)
{
    va_list(args);
    va_start(args, format);
    vprintf(format, args);
}

uint32_t esp_log_timestamp(void)
{
    return 0;
}
