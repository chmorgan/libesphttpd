#ifndef HTTPDESPFS_H
#define HTTPDESPFS_H

#ifdef linux
#include <libesphttpd/linux.h>
#else
#include <libesphttpd/esp.h>  // for sdkconfig.h
#endif

#ifdef CONFIG_ESPHTTPD_USE_ESPFS
#include "httpd.h"
/**
 * The template substitution callback.
 * Returns CGI_MORE if more should be sent within the token, CGI_DONE otherwise.
 */
typedef CgiStatus (* TplCallback)(HttpdConnData *connData, char *token, void **arg);

CgiStatus cgiEspFsHook(HttpdConnData *connData);
CgiStatus ICACHE_FLASH_ATTR cgiEspFsTemplate(HttpdConnData *connData);

/**
 * @return 1 upon success, 0 upon failure
 */
int tplSend(HttpdConnData *conn, const char *str, int len);

#endif // CONFIG_ESPHTTPD_USE_ESPFS

#endif // HTTPDESPFS_H
