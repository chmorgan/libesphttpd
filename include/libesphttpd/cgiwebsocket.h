#ifndef CGIWEBSOCKET_H
#define CGIWEBSOCKET_H

#include "httpd.h"
#include "kref.h"

#define WEBSOCK_FLAG_NONE 0
#define WEBSOCK_FLAG_MORE (1<<0) //Set if the data is not the final data in the message; more follows
#define WEBSOCK_FLAG_BIN (1<<1) //Set if the data is binary instead of text
#define WEBSOCK_FLAG_CONT (1<<2) //set if this is a continuation frame (after WEBSOCK_FLAG_CONT)
#define WEBSOCK_CLOSED -1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Websock Websock;
typedef struct WebsockPriv WebsockPriv;

typedef void(*WsConnectedCb)(Websock *ws);
typedef void(*WsRecvCb)(Websock *ws, char *data, int len, int flags);
typedef void(*WsSentCb)(Websock *ws);
typedef void(*WsCloseCb)(Websock *ws);

struct Websock {
	struct kref ref_cnt; // reference count to manage lifetime of this shared object
	void *userData; // optional user data to attach to a Websock object, not used by the library
	HttpdConnData *conn; // Stores a reference to the connData, but warning that we don't own it and it may be freed. 
	WsRecvCb recvCb; // optional user callback on data recieved
	WsSentCb sentCb; // optional user callback on data sent
	WsCloseCb closeCb; // optional user callback on websocket close
	WebsockPriv *priv;
};

CgiStatus ICACHE_FLASH_ATTR cgiWebsocket(HttpdConnData *connData);
int ICACHE_FLASH_ATTR cgiWebsocketSend(HttpdInstance *pInstance, Websock *ws, const char *data, int len, int flags);
void ICACHE_FLASH_ATTR cgiWebsocketClose(HttpdInstance *pInstance, Websock *ws, int reason);
CgiStatus ICACHE_FLASH_ATTR cgiWebSocketRecv(HttpdInstance *pInstance, HttpdConnData *connData, char *data, int len);
int ICACHE_FLASH_ATTR cgiWebsockBroadcast(HttpdInstance *pInstance, const char *resource, const char *data, int len, int flags);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
