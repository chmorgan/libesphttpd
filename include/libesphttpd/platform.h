#pragma once

#ifndef LINUX

#include <libesphttpd/esp.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

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

#else

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

#endif
