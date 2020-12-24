// Combined include file for esp8266 and esp32

#ifdef ESP_PLATFORM //only set in esp-idf
#define FREERTOS 1
#define ESP32 1

#include "sdkconfig.h"
#define HTTPD_STACKSIZE CONFIG_ESPHTTPD_STACK_SIZE
#include "stdint.h"
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;

#define ICACHE_RODATA_ATTR
#endif


#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
# include "esp_attr.h"
# include "esp_log.h"
# include "esp_types.h"
# if defined(CONFIG_IDF_TARGET_ESP32) || \
     defined(CONFIG_IDF_TARGET_ESP32S2) || \
     defined(CONFIG_IDF_TARGET_ESP32S3)
#  include "esp_spi_flash.h"
# endif
#endif

#include "platform.h"
