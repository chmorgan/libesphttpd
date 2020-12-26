#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef FREERTOS
#include <libesphttpd/esp.h>

#ifdef ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#else
#include "FreeRTOS.h"
#include "timers.h"
#endif

//#include "esp_timer.h"
typedef struct RtosConnType RtosConnType;
typedef RtosConnType* ConnTypePtr;

#ifdef ESP32
// freertos v8 api
typedef TimerHandle_t HttpdPlatTimerHandle;
#else
// freertos v7 api
typedef xTimerHandle HttpdPlatTimerHandle;
#endif

#elif defined(linux)

#include <unistd.h>
#include <stdbool.h>
typedef struct RtosConnType RtosConnType;
typedef RtosConnType* ConnTypePtr;

#define vTaskDelay(milliseconds) usleep((milliseconds) * 1000)
#define portTICK_RATE_MS 1
#define portTICK_PERIOD_MS 1

typedef struct
{
	timer_t timer;
	int timerPeriodMS;
	bool autoReload;
	void (*callback)(void* arg);
	void* callbackArg;
} HttpdPlatTimer;

typedef HttpdPlatTimer* HttpdPlatTimerHandle;

#else // no-os, map to os-specific versions that have to be defined

typedef struct
{
	os_timer_t timer;
	int timerPeriodMS;
	bool autoReload;
} HttpdPlatTimer;

#define printf(...) os_printf(__VA_ARGS__)
#define sprintf(str, ...) os_sprintf(str, __VA_ARGS__)
#define strcpy(a, b) os_strcpy(a, b)
#define strncpy(a, b, c) os_strncpy(a, b, c)
#define strcmp(a, b) os_strcmp(a, b)
#define strncmp(a, b, c) os_strncmp(a, b, c)
#define malloc(x) os_malloc(x)
#define free(x) os_free(x)
#define memset(x, a, b) os_memset(x, a, b)
#define memcpy(x, a, b) os_memcpy(x, a, b)
#define strcat(a, b) os_strcat(a, b)
#define strstr(a, b) os_strstr(a, b)
#define strlen(a) os_strlen(a)
#define memcmp(a, b, c) os_memcmp(a, b, c)
typedef struct espconn* ConnTypePtr;
typedef HttpdPlatTimer* HttpdPlatTimerHandle;
#define httpd_printf(format, ...) os_printf(format, ##__VA_ARGS__)
#endif



#endif
