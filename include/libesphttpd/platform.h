#pragma once

#ifndef LINUX
# include <freertos/FreeRTOS.h>
# include <freertos/timers.h>

typedef struct RtosConnType RtosConnType;
typedef RtosConnType* ConnTypePtr;
typedef TimerHandle_t HttpdPlatTimerHandle;

#else
# include <unistd.h>
# include <stdbool.h>
# include <sys/types.h>

typedef struct RtosConnType RtosConnType;
typedef RtosConnType* ConnTypePtr;

# define vTaskDelay(milliseconds) usleep((milliseconds) * 1000)
# define portTICK_RATE_MS 1
# define portTICK_PERIOD_MS 1

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

#include "libesphttpd/httpd.h"

/**
 * @return number of bytes that were written
 */
int httpdPlatSendData(HttpdInstance *pInstance, HttpdConnData *pConn, char *buff, int len);

void httpdPlatDisconnect(HttpdConnData *ponn);
void httpdPlatDisableTimeout(HttpdConnData *pConn);

void httpdPlatLock(HttpdInstance *pInstance);
void httpdPlatUnlock(HttpdInstance *pInstance);

HttpdPlatTimerHandle httpdPlatTimerCreate(const char *name, int periodMs, int autoreload, void (*callback)(void *arg), void *ctx);
void httpdPlatTimerStart(HttpdPlatTimerHandle timer);
void httpdPlatTimerStop(HttpdPlatTimerHandle timer);
void httpdPlatTimerDelete(HttpdPlatTimerHandle timer);

#ifdef CONFIG_ESPHTTPD_SHUTDOWN_SUPPORT
void httpdPlatShutdown(HttpdInstance *pInstance);
#endif
