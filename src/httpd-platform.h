#ifndef HTTPD_PLATFORM_H
#define HTTPD_PLATFORM_H

#include "libesphttpd/platform.h"

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

#endif
