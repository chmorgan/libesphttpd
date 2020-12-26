#pragma once

#ifndef LINUX
# include <esp_event.h>
#endif

#include "libesphttpd/httpd.h"


#ifdef __cplusplus
extern "C" {
#endif

//Parameter given to cgiAuthBasic. This callback returns the usernames/passwords the device
//has.
typedef int (* AuthGetUserPw)(HttpdConnData *connData, int no, char *user, int userLen, char *pass, int passLen);

CgiStatus cgiAuthBasic(HttpdConnData *connData);

CgiStatus cgiGetFirmwareNext(HttpdConnData *connData);
CgiStatus cgiUploadFirmware(HttpdConnData *connData);
CgiStatus cgiRebootFirmware(HttpdConnData *connData);
CgiStatus cgiSetBoot(HttpdConnData *connData);
CgiStatus cgiEraseFlash(HttpdConnData *connData);
CgiStatus cgiGetFlashInfo(HttpdConnData *connData);


CgiStatus cgiRedirect(HttpdConnData *connData);

// This CGI function redirects to a fixed url of http://[hostname]/ if hostname
// field of request isn't already that hostname. Use this in combination with
// a DNS server that redirects everything to the ESP in order to load a HTML
// page as soon as a phone, tablet etc connects to the ESP.
//
// NOTE: If your httpd server is listening on all interfaces this will also
//       redirect connections when the ESP is in STA mode, potentially to a
//       hostname that is not in the 'official' DNS and so will fail.
//
// It is recommended to place this cgi route early or first in your route list
// so the redirect occurs before other route processing
//
// The 'cgiArg' to this cgi function is the hostname that the client will be redirected
// to.
//
// If the hostname matches the hostname specified in cgiArg then this cgi function
// will return HTTPD_CGI_NOTFOUND, causing the libesphttpd server to skip
// over this cgi function and to continue processing with the next route
//
// Example usage:
//         ROUTE_CGI_ARG("*", cgiRedirectToHostname, "HOSTNAME_HERE"),
CgiStatus cgiRedirectToHostname(HttpdConnData *connData);

CgiStatus cgiRedirectApClientToHostname(HttpdConnData *connData);

#define WEBSOCK_FLAG_NONE 0
#define WEBSOCK_FLAG_MORE (1<<0) //Set if the data is not the final data in the message; more follows
#define WEBSOCK_FLAG_BIN (1<<1) //Set if the data is binary instead of text
#define WEBSOCK_FLAG_CONT (1<<2) //set if this is a continuation frame (after WEBSOCK_FLAG_CONT)
#define WEBSOCK_CLOSED -1

typedef struct Websock Websock;
typedef struct WebsockPriv WebsockPriv;

typedef void(*WsConnectedCb)(Websock *ws);
typedef void(*WsRecvCb)(Websock *ws, char *data, int len, int flags);
typedef void(*WsSentCb)(Websock *ws);
typedef void(*WsCloseCb)(Websock *ws);

struct Websock {
	void *userData;
	HttpdConnData *conn;
	uint8_t status;
	WsRecvCb recvCb;
	WsSentCb sentCb;
	WsCloseCb closeCb;
	WebsockPriv *priv;
};

CgiStatus cgiWebsocket(HttpdConnData *connData);
int cgiWebsocketSend(HttpdInstance *pInstance, Websock *ws, const char *data, int len, int flags);
void cgiWebsocketClose(HttpdInstance *pInstance, Websock *ws, int reason);
CgiStatus cgiWebSocketRecv(HttpdInstance *pInstance, HttpdConnData *connData, char *data, int len);
int cgiWebsockBroadcast(HttpdInstance *pInstance, const char *resource, char *data, int len, int flags);

#ifndef LINUX
CgiStatus cgiWiFiScan(HttpdConnData *connData);
CgiStatus tplWlan(HttpdConnData *connData, char *token, void **arg);
CgiStatus cgiWiFi(HttpdConnData *connData);
CgiStatus cgiWiFiConnect(HttpdConnData *connData);
CgiStatus cgiWiFiSetMode(HttpdConnData *connData);
CgiStatus cgiWiFiAPSettings(HttpdConnData *connData);
CgiStatus cgiWiFiConnStatus(HttpdConnData *connData);

esp_err_t initCgiWifi(void);
void cgiWifiEventCb(system_event_t *event);
CgiStatus cgiWiFiStartWps(HttpdConnData *connData);
#endif

//This is a catch-all cgi function. It takes the url passed to it, looks up the corresponding
//path in the filesystem and if it exists, passes the file through. This simulates what a normal
//webserver would do with static files.
// If the file is not found, (or if http method is not GET) this cgi function returns NOT_FOUND, and then other cgi functions specified later in the routing table can try.
// The cgiArg value is the base directory path, if specified.
//
// Usage:
//      ROUTE_CGI("*", cgiEspVfsGet) or
//      ROUTE_CGI_ARG("*", cgiEspVfsGet, "/base/directory/") or
//      ROUTE_CGI_ARG("*", cgiEspVfsGet, ".") to use the current working directory
CgiStatus cgiEspVfsGet(HttpdConnData *connData);
CgiStatus cgiEspVfsTemplate(HttpdConnData *connData);

//This is a POST and PUT handler for uploading files to the VFS filesystem.
// If http method is not PUT or POST, this cgi function returns NOT_FOUND, and then other cgi functions specified later in the routing table can try.
// Specify base directory (with trailing slash) or single file as 1st cgiArg.
//
// Filename can be specified 3 ways, in order of priority lowest to highest:
//  1.  URL Path.  i.e. PUT http://1.2.3.4/path/newfile.txt
//  2.  Inside multipart/form-data (todo not supported yet)
//  3.  URL Parameter.  i.e. POST http://1.2.3.4/upload.cgi?filename=path%2Fnewfile.txt
//
// Usage:
//      ROUTE_CGI_ARG("*", cgiEspVfsUpload, "/base/directory/")
//           - Allows creating/replacing files anywhere under "/base/directory/".  Don't forget to specify trailing slash in cgiArg!
//           - example: POST or PUT http://1.2.3.4/anydir/anyname.txt
//
//      ROUTE_CGI_ARG("/filesystem/upload.cgi", cgiEspVfsUpload, "/base/directory/")
//           - Allows creating/replacing files anywhere under "/base/directory/".  Don't forget to specify trailing slash in cgiArg!
//           - example: POST or PUT http://1.2.3.4/filesystem/upload.cgi?filename=newdir%2Fnewfile.txt
//
//      ROUTE_CGI_ARG("/writeable_file.txt", cgiEspVfsUpload, "/base/directory/writeable_file.txt")
//           - Allows only replacing content of one file at "/base/directory/writeable_file.txt".
//           - example: POST or PUT http://1.2.3.4/writeable_file.txt
CgiStatus cgiEspVfsUpload(HttpdConnData *connData);

#ifdef __cplusplus
} /* extern "C" */
#endif
