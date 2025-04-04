/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Websocket support for esphttpd. Inspired by https://github.com/dangrie158/ESP-8266-WebSocket
*/

#ifdef linux
#include <libesphttpd/linux.h>
#else
#include <libesphttpd/esp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#include "libesphttpd/httpd.h"
#include "httpd-platform.h"
#include "libesphttpd/sha1.h"
#include "libesphttpd_base64.h"
#include "libesphttpd/cgiwebsocket.h"

//#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
const static char* TAG = "cgiwebsocket";

#define WS_KEY_IDENTIFIER "Sec-WebSocket-Key: "
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* from IEEE RFC6455 sec 5.2
      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-------+-+-------------+-------------------------------+
     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
     | |1|2|3|       |K|             |                               |
     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
     |     Extended payload length continued, if payload len == 127  |
     + - - - - - - - - - - - - - - - +-------------------------------+
     |                               |Masking-key, if MASK set to 1  |
     +-------------------------------+-------------------------------+
     | Masking-key (continued)       |          Payload Data         |
     +-------------------------------- - - - - - - - - - - - - - - - +
     :                     Payload Data continued ...                :
     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
     |                     Payload Data continued ...                |
     +---------------------------------------------------------------+
*/

#define FLAG_FIN (1 << 7)

#define OPCODE_CONTINUE 0x0
#define OPCODE_TEXT 0x1
#define OPCODE_BINARY 0x2
#define OPCODE_CLOSE 0x8
#define OPCODE_PING 0x9
#define OPCODE_PONG 0xA

#define FLAGS_MASK ((uint8_t)0xF0)
#define OPCODE_MASK ((uint8_t)0x0F)
#define IS_MASKED ((uint8_t)(1<<7))
#define PAYLOAD_MASK ((uint8_t)0x7F)

typedef struct WebsockFrame WebsockFrame;

#define ST_FLAGS 0
#define ST_LEN0 1
#define ST_LEN1 2
#define ST_LEN2 3
//...
#define ST_LEN8 9
#define ST_MASK1 10
#define ST_MASK4 13
#define ST_PAYLOAD 14

struct WebsockFrame {
	uint8_t flags;
	uint8_t len8;
	uint64_t len;
	uint8_t mask[4];
};

struct WebsockPriv {
	struct WebsockFrame fr;
	uint8_t maskCtr;
	uint8 frameCont;
	int wsStatus;
};


#define WEBSOCK_LIST_SIZE (32)
// List of active Websockets (TODO: not support multiple httpd instances yet)
static Websock *wsList[WEBSOCK_LIST_SIZE] = {NULL};
// Recursive mutex to protect list
static SemaphoreHandle_t wsListMutex=NULL;

// Lock access to list
static bool ICACHE_FLASH_ATTR wsListLock(void)
{
	if (wsListMutex == NULL) {
		wsListMutex = xSemaphoreCreateRecursiveMutex();
	}
	assert(wsListMutex != NULL);
	ESP_LOGD(TAG, "Lock list. Task: %p", xTaskGetCurrentTaskHandle());
	// Take the mutex recursively
	if (pdTRUE != xSemaphoreTakeRecursive(wsListMutex, portMAX_DELAY)) {
		ESP_LOGE(TAG, "Failed to take mutex");
		return false;
	}
	return true;
}

// Unlock list access
static void ICACHE_FLASH_ATTR wsListUnlock(void)
{
	assert(wsListMutex != NULL);
	ESP_LOGD(TAG, "Unlock list. Task: %p", xTaskGetCurrentTaskHandle());
	// Give the mutex recursively
	xSemaphoreGiveRecursive(wsListMutex);
}

static bool check_websock_closed(Websock *ws) {
    return ((NULL == ws->conn) || (ws->conn->isConnectionClosed));
}

/* Free data, must only be called by kref_put()! */
static void ICACHE_FLASH_ATTR free_websock(struct kref *ref)
{
	ESP_LOGD(TAG, "websockFree");
	Websock *ws;

	ws = kcontainer_of(ref, Websock, ref_cnt);
	if (ws->priv) free(ws->priv);
	// Note we don't free ws->conn, since it is managed by the httpd instance.
	free(ws);
}

/* Drop a reference to a Websock, possibly freeing it. */
static void put_websock(Websock *ws)
{
	assert(ws != NULL);
	kref_put(&(ws->ref_cnt), free_websock);
}

/* Get a reference counted pointer to a Websock object by index. *\
\* Must be released via put_websock()!                           */
static struct Websock *get_websock(unsigned int index)
{
	Websock *retWs = NULL;
	if ((index < WEBSOCK_LIST_SIZE) && 
		(NULL != wsList[index])) // do an early NULL check to avoid locking an empty slot
	{
		if (wsListLock())
		{
			if (wsList[index] != NULL) // double check for NULL now that we have the lock
			{
				// check if the websocket is closed and remove it from the list
				// (Optional to do it here, since it is also done in wsListAddAndGC) 
				if (check_websock_closed(wsList[index])) {
					ESP_LOGD(TAG, "Cleaning up websocket %p", wsList[index]);
					put_websock(wsList[index]); // decrement reference count for the ref in the list
					wsList[index] = NULL; // remove from list
					// will return NULL to indicate closed
				} else { 	
					// ws is still open
					retWs = wsList[index];
					// increment reference count, since we will be retuning a pointer to it
					kref_get(&(retWs->ref_cnt));
				}
			}
			wsListUnlock();
		}
	}
	return retWs;
}

// Add websocket to list, and garbage collect dead websockets
static void ICACHE_FLASH_ATTR wsListAddAndGC(Websock *ws)
{
	ESP_LOGD(TAG, "Adding to list. Task: %p", xTaskGetCurrentTaskHandle());
	if (wsListLock()) // protect access to list, since it could be used/modified outside this thread
	{
		// Garbage collection
		for (int i = 0; i < WEBSOCK_LIST_SIZE; i++) {
			if (wsList[i] != NULL) {
				if (check_websock_closed(wsList[i])) {
					ESP_LOGD(TAG, "Cleaning up websocket %p", wsList[i]);
					put_websock(wsList[i]); // decrement reference count
					wsList[i] = NULL;
				}
			}
		}
		// Insert ws into list
		int j = 0;
		// Find first empty slot
		for (; j < WEBSOCK_LIST_SIZE; j++) {
			// Check if slot is empty
			if (wsList[j] == NULL) {
				ESP_LOGD(TAG, "Adding websocket %p to slot %d", ws, j);
				kref_get(&(ws->ref_cnt)); // increment reference count
				wsList[j] = ws;
				break;
			}
		}
		if (j == WEBSOCK_LIST_SIZE) {
			ESP_LOGE(TAG, "Websocket list full");
		}
		wsListUnlock();
	}
}

static int ICACHE_FLASH_ATTR sendFrameHead(Websock *ws, int opcode, int len) {
	char buf[14];
	int i=0;
	buf[i++]=opcode;
	if (len>65535) {
		buf[i++]=127;
		buf[i++]=0; buf[i++]=0; buf[i++]=0; buf[i++]=0;
		buf[i++]=len>>24;
		buf[i++]=len>>16;
		buf[i++]=len>>8;
		buf[i++]=len;
	} else if (len>125) {
		buf[i++]=126;
		buf[i++]=len>>8;
		buf[i++]=len;
	} else {
		buf[i++]=len;
	}
	ESP_LOGD(TAG, "Sent frame head for payload of %d bytes", len);
	return httpdSend(ws->conn, buf, i);
}

int ICACHE_FLASH_ATTR cgiWebsocketSend(HttpdInstance *pInstance, Websock *ws, const char *data, int len, int flags) {
	int r=0;
	int fl=0;

	// Continuation frame has opcode 0
	if (!(flags&WEBSOCK_FLAG_CONT)) {
		if (flags & WEBSOCK_FLAG_BIN)
			fl = OPCODE_BINARY;
		else
			fl = OPCODE_TEXT;
	}
	// add FIN to last frame
	if (!(flags&WEBSOCK_FLAG_MORE)) fl|=FLAG_FIN;

	httpdPlatLock(pInstance);
	if (check_websock_closed(ws)) {
		ESP_LOGE(TAG, "Websocket closed, cannot send");
		httpdPlatUnlock(pInstance);
		return WEBSOCK_CLOSED;
	}
	sendFrameHead(ws, fl, len);
	if (len!=0) r=httpdSend(ws->conn, data, len);
	httpdFlushSendBuffer(pInstance, ws->conn);
	httpdPlatUnlock(pInstance);
	return r;
}

//Broadcast data to all websockets at a specific url. Returns the amount of connections sent to.
int ICACHE_FLASH_ATTR cgiWebsockBroadcast(HttpdInstance *pInstance, const char *resource, const char *data, int len, int flags) {
	int ret = 0;

	for (int i = 0; i < WEBSOCK_LIST_SIZE; i++) {
		Websock *ws = get_websock(i); // get a reference counted pointer to the Websock object from the list
		if (NULL != ws) {
			httpdPlatLock(pInstance); // lock needed so we can access the connData to check for routeMatch
			if (check_websock_closed(ws)) {
				ESP_LOGD(TAG, "Websocket %p closed", ws);
				httpdPlatUnlock(pInstance);
				continue;
			}
			// else ws is still open
			bool routeMatch = (strcmp(ws->conn->url, resource) == 0);
			httpdPlatUnlock(pInstance);

			if (routeMatch) {
				cgiWebsocketSend(pInstance, ws, data, len, flags);
				ret++;
			}
			put_websock(ws); // decrement reference count for local copy
		}
	}
	if (ret == 0) {
		ESP_LOGD(TAG, "No websockets found for resource %s", resource);
	}
	else {
		ESP_LOGD(TAG, "Broadcasted %d bytes to %d websockets", len, ret);
	}
	return ret;
}


void ICACHE_FLASH_ATTR cgiWebsocketClose(HttpdInstance *pInstance, Websock *ws, int reason) {
	char rs[2]={reason>>8, reason&0xff};
	httpdPlatLock(pInstance);
	if (ws->conn != NULL) {
		sendFrameHead(ws, FLAG_FIN|OPCODE_CLOSE, 2);
		httpdSend(ws->conn, rs, 2);
		httpdFlushSendBuffer(pInstance, ws->conn);
		ws->conn = NULL; // mark as closed for shared references
        if (ws->closeCb) ws->closeCb(ws);
	} // else already closed
	httpdPlatUnlock(pInstance);
}

CgiStatus ICACHE_FLASH_ATTR cgiWebSocketRecv(HttpdInstance *pInstance, HttpdConnData *connData, char *data, int len) {
	int i, j, sl;
	int r=HTTPD_CGI_MORE;
	int wasHeaderByte;
	Websock *ws=(Websock*)connData->cgiData;
	assert(ws != NULL);
	kref_get(&(ws->ref_cnt)); // increment reference count
	for (i=0; i<len; i++) {
//		httpd_printf("Ws: State %d byte 0x%02X\n", ws->priv->wsStatus, data[i]);
		wasHeaderByte=1;
		if (ws->priv->wsStatus==ST_FLAGS) {
			ws->priv->maskCtr=0;
			ws->priv->frameCont=0;
			ws->priv->fr.flags=(uint8_t)data[i];
			ws->priv->wsStatus=ST_LEN0;
		} else if (ws->priv->wsStatus==ST_LEN0) {
			ws->priv->fr.len8=(uint8_t)data[i];
			if ((ws->priv->fr.len8&127)>=126) {
				ws->priv->fr.len=0;
				ws->priv->wsStatus=ST_LEN1;
			} else {
				ws->priv->fr.len=ws->priv->fr.len8&127;
				ws->priv->wsStatus=(ws->priv->fr.len8&IS_MASKED)?ST_MASK1:ST_PAYLOAD;
			}
		} else if (ws->priv->wsStatus<=ST_LEN8) {
			ws->priv->fr.len=(ws->priv->fr.len<<8)|data[i];
			if (((ws->priv->fr.len8&127)==126 && ws->priv->wsStatus==ST_LEN2) || ws->priv->wsStatus==ST_LEN8) {
				ws->priv->wsStatus=(ws->priv->fr.len8&IS_MASKED)?ST_MASK1:ST_PAYLOAD;
			} else {
				ws->priv->wsStatus++;
			}
		} else if (ws->priv->wsStatus<=ST_MASK4) {
			ws->priv->fr.mask[ws->priv->wsStatus-ST_MASK1]=data[i];
			ws->priv->wsStatus++;
		} else {
			//Was a payload byte.
			wasHeaderByte=0;
		}

		if (ws->priv->wsStatus==ST_PAYLOAD && wasHeaderByte) {
			//We finished parsing the header, but i still is on the last header byte. Move one forward so
			//the payload code works as usual.
			i++;
		}
		//Also finish parsing frame if we haven't received any payload bytes yet, but the length of the frame
		//is zero.
		if (ws->priv->wsStatus==ST_PAYLOAD) {
			//Okay, header is in; this is a data byte. We're going to process all the data bytes we have
			//received here at the same time; no more byte iterations till the end of this frame.
			//First, unmask the data
			sl=len-i;
			ESP_LOGD(TAG, "Frame payload. wasHeaderByte %d fr.len %d sl %d cmd 0x%x", wasHeaderByte, (int)ws->priv->fr.len, (int)sl, ws->priv->fr.flags);
			if (sl > ws->priv->fr.len) sl=ws->priv->fr.len;
			for (j=0; j<sl; j++) data[i+j]^=(ws->priv->fr.mask[(ws->priv->maskCtr++)&3]);

//			httpd_printf("Unmasked: ");
//			for (j=0; j<sl; j++) httpd_printf("%02X ", data[i+j]&0xff);
//			httpd_printf("\n");

			//Inspect the header to see what we need to do.
			if ((ws->priv->fr.flags&OPCODE_MASK)==OPCODE_PING) {
				if (ws->priv->fr.len>125) {
					if (!ws->priv->frameCont) cgiWebsocketClose(pInstance, ws, 1002);
					r=HTTPD_CGI_DONE;
					break;
				} else {
					if (!ws->priv->frameCont) sendFrameHead(ws, OPCODE_PONG|FLAG_FIN, ws->priv->fr.len);
					if (sl>0) httpdSend(ws->conn, data+i, sl);
				}
			} else if ((ws->priv->fr.flags&OPCODE_MASK)==OPCODE_TEXT ||
						(ws->priv->fr.flags&OPCODE_MASK)==OPCODE_BINARY ||
						(ws->priv->fr.flags&OPCODE_MASK)==OPCODE_CONTINUE) {
				if (sl>ws->priv->fr.len) sl=ws->priv->fr.len;
				if (!(ws->priv->fr.len8&IS_MASKED)) {
					//We're a server; client should send us masked packets.
					cgiWebsocketClose(pInstance, ws, 1002);
					r=HTTPD_CGI_DONE;
					break;
				} else {
					int flags=0;
					if ((ws->priv->fr.flags&OPCODE_MASK)==OPCODE_BINARY) flags|=WEBSOCK_FLAG_BIN;
					if ((ws->priv->fr.flags&FLAG_FIN)==0) flags|=WEBSOCK_FLAG_MORE;
					if (ws->recvCb) ws->recvCb(ws, data+i, sl, flags);
				}
			} else if ((ws->priv->fr.flags&OPCODE_MASK)==OPCODE_CLOSE) {
				ESP_LOGD(TAG, "Got close frame");
				cgiWebsocketClose(pInstance, ws, ((data[i]<<8)&0xff00)+(data[i+1]&0xff));
				r=HTTPD_CGI_DONE;
				break;
			} else {
				if (!ws->priv->frameCont) ESP_LOGE(TAG, "Unknown opcode 0x%X", ws->priv->fr.flags&OPCODE_MASK);
			}
			i+=sl-1;
			ws->priv->fr.len-=sl;
			if (ws->priv->fr.len==0) {
				ws->priv->wsStatus=ST_FLAGS; //go receive next frame
			} else {
				ws->priv->frameCont=1; //next payload is continuation of this frame.
			}
		}
	}
	if (r==HTTPD_CGI_DONE) {
		//We're going to tell the main webserver we're done. The webserver expects us to clean up by ourselves
		//we're chosing to be done. Do so.
		put_websock((Websock*)connData->cgiData); // drop reference for connData->cgiData
		connData->cgiData=NULL;
	}
	put_websock(ws); // drop reference for local ws, possibly free it if no one else is using it
	return r;
}

//Websocket 'cgi' implementation
CgiStatus ICACHE_FLASH_ATTR cgiWebsocket(HttpdConnData *connData) {
	char buff[256];
	int i;
	sha1nfo s;
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		ESP_LOGD(TAG, "Cleanup");
		if (connData->cgiData) {
			((Websock*)connData->cgiData)->conn = NULL; // mark as closed for shared references
			put_websock((Websock*)connData->cgiData); // drop reference for connData->cgiData
			connData->cgiData=NULL;
		}
		return HTTPD_CGI_DONE;
	}

	if (connData->cgiData==NULL) {
		ESP_LOGV(TAG, "WS: First call");
		//First call here. Check if client headers are OK, send server header.
		i=httpdGetHeader(connData, "Upgrade", buff, sizeof(buff)-1);
		ESP_LOGD(TAG, "Upgrade: %s", buff);
		if (i && strcasecmp(buff, "websocket")==0) {
			i=httpdGetHeader(connData, "Sec-WebSocket-Key", buff, sizeof(buff)-1);
			if (i) {
				ESP_LOGV(TAG, "WS: Key: %s", buff);
				//Seems like a WebSocket connection.
				// Alloc structs
				Websock *ws = calloc(1, sizeof(Websock));
				if (ws == NULL) {
					ESP_LOGE(TAG, "Can't allocate mem for websocket");
					return HTTPD_CGI_DONE;
				}
				kref_init(&(ws->ref_cnt)); // initialises ref_cnt to 1
				ws->priv = calloc(1, sizeof(WebsockPriv));
				if (ws->priv==NULL) {
					ESP_LOGE(TAG, "Can't allocate mem for websocket priv");
					if (ws != NULL)
					{
						put_websock(ws); // drop reference to local ws
					}
					return HTTPD_CGI_DONE;
				}

				// Store a reference to the connData, but note that ws doesn't own it. 
				// We have to be careful using it because it can be freed by the httpd instance.
				ws->conn=connData;
				//Reply with the right headers.
				strcat(buff, WS_GUID);
				sha1_init(&s);
				sha1_write(&s, buff, strlen(buff));
				httpdSetTransferMode(connData, HTTPD_TRANSFER_NONE);
				httpdStartResponse(connData, 101);
				httpdHeader(connData, "Upgrade", "websocket");
				httpdHeader(connData, "Connection", "upgrade");
				libesphttpd_base64_encode(20, sha1_result(&s), sizeof(buff), buff);
				httpdHeader(connData, "Sec-WebSocket-Accept", buff);
				httpdEndHeaders(connData);
				//Set data receive handler
				connData->recvHdl=cgiWebSocketRecv;
				//Inform CGI function we have a connection
				WsConnectedCb connCb=connData->cgiArg;
				connCb(ws);

				// Add a reference count, since it is stored on connData->cgiData until the connection is closed
				kref_get(&(ws->ref_cnt));
				connData->cgiData = ws;

				wsListAddAndGC(ws); // add to list, will add another reference count

				put_websock(ws); // drop reference to local ws
				return HTTPD_CGI_MORE;
			}
		}
		//No valid websocket connection
		httpdStartResponse(connData, 500);
		httpdEndHeaders(connData);
		return HTTPD_CGI_DONE;
	}

	//Sending is done. Call the sent callback if we have one.
	Websock *ws=(Websock*)connData->cgiData;
	if (ws && ws->sentCb) ws->sentCb(ws);

	return HTTPD_CGI_MORE;
}
