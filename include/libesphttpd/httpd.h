#ifndef HTTPD_H
#define HTTPD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "esp.h"

/**
 * Design notes:
 *  - The platform code owns the memory management of connections
 *  - The platform embeds a HttpdConnData into its own structure, this enables
 *    the platform code, through the use of the container_of approach, to
 *    find its connection structure when given a HttpdConnData*
 */


#define HTTPDVER "0.5"

//Max length of request head. This is statically allocated for each connection.
#ifndef HTTPD_MAX_HEAD_LEN
#define HTTPD_MAX_HEAD_LEN		1024
#endif

//Max post buffer len. This is dynamically malloc'ed if needed.
#ifndef HTTPD_MAX_POST_LEN
#define HTTPD_MAX_POST_LEN		2048
#endif

//Send buffer size
#ifndef HTTPD_SENDBUFF_SIZE
#define HTTPD_SENDBUFF_SIZE	2048
#endif

//Send buffer limit for httpdSend. 2 bytes are reserved for chunk termination ('\r\n').
#ifndef HTTPD_SENDBUFF_MAX_FILL
#define HTTPD_SENDBUFF_MAX_FILL	(HTTPD_SENDBUFF_SIZE - 2)
#endif

//Send buffer limit (for backward compatibility
#ifndef HTTPD_MAX_SENDBUFF_LEN
#define HTTPD_MAX_SENDBUFF_LEN HTTPD_SENDBUFF_MAX_FILL
#endif

//If some data can't be sent because the underlaying socket doesn't accept the data (like the nonos
//layer is prone to do), we put it in a backlog that is dynamically malloc'ed. This defines the max
//size of the backlog.
#ifndef HTTPD_MAX_BACKLOG_SIZE
#define HTTPD_MAX_BACKLOG_SIZE	(4*1024)
#endif

//Max length of CORS token. This amount is allocated per connection.
#define MAX_CORS_TOKEN_LEN 256

typedef enum
{
	HTTPD_CGI_MORE,
	HTTPD_CGI_DONE,
	HTTPD_CGI_NOTFOUND,
	HTTPD_CGI_AUTHENTICATED
} CgiStatus;

typedef enum
{
	HTTPD_METHOD_GET,
	HTTPD_METHOD_POST,
	HTTPD_METHOD_OPTIONS,
	HTTPD_METHOD_PUT,
	HTTPD_METHOD_PATCH,
	HTTPD_METHOD_DELETE
} RequestTypes;

typedef enum
{
	HTTPD_TRANSFER_CLOSE,
	HTTPD_TRANSFER_CHUNKED,
	HTTPD_TRANSFER_NONE
} TransferModes;

typedef struct HttpdPriv HttpdPriv;
typedef struct HttpdConnData HttpdConnData;
typedef struct HttpdPostData HttpdPostData;
typedef struct HttpdInstance HttpdInstance;


typedef CgiStatus (* cgiSendCallback)(HttpdConnData *connData);
typedef CgiStatus (* cgiRecvHandler)(HttpdInstance *pInstance, HttpdConnData *connData, char *data, int len);

#ifdef CONFIG_ESPHTTPD_BACKLOG_SUPPORT
struct HttpSendBacklogItem {
	int len;
	HttpSendBacklogItem *next;
	char data[];
};
#endif

//Private data for http connection
struct HttpdPriv {
	char head[HTTPD_MAX_HEAD_LEN];
#ifdef CONFIG_ESPHTTPD_CORS_SUPPORT
	char corsToken[MAX_CORS_TOKEN_LEN];
#endif
	int headPos;
	char sendBuff[HTTPD_SENDBUFF_SIZE];
	int sendBuffLen;

	/** NOTE: chunkHdr, if valid, points at memory assigned to sendBuff
		so it doesn't have to be freed */
	char *chunkHdr;

#ifdef CONFIG_ESPHTTPD_BACKLOG_SUPPORT
	HttpSendBacklogItem *sendBacklog;
	int sendBacklogSize;
#endif
	int flags;
};

//A struct describing the POST data sent inside the http connection.  This is used by the CGI functions
struct HttpdPostData {
	int len;				// POST Content-Length
	int buffSize;			// The maximum length of the post buffer
	int buffLen;			// The amount of bytes in the current post buffer
	int received;			// The total amount of bytes received so far
	char *buff;				// Actual POST data buffer
	char *multipartBoundary; // Pointer to the start of the multipart boundary value in priv.head
};

//A struct describing a http connection. This gets passed to cgi functions.
struct HttpdConnData {
	RequestTypes requestType;
	char *url;				// The URL requested, without hostname or GET arguments
	const char *route;		// The route matched.
	char *getArgs;			// The GET arguments for this request, if any.
	const void *cgiArg;		// Argument to the CGI function, as stated as the 3rd argument of
							// the builtInUrls entry that referred to the CGI function.
	const void *cgiArg2;	// 4th argument of the builtInUrls entries, used to pass template file to the tpl handler.
	void *cgiData;			// Opaque data pointer for the CGI function
	char *hostName;			// Host name field of request
	HttpdPriv priv;		// Data for internal httpd housekeeping
	cgiSendCallback cgi;	// CGI function pointer
	cgiRecvHandler recvHdl;	// Handler for data received after headers, if any
	HttpdPostData post;	// POST data structure
	bool isConnectionClosed;
};

//A struct describing an url. This is the main struct that's used to send different URL requests to
//different routines.
typedef struct {
	const char *url;
	cgiSendCallback cgiCb;
	const void *cgiArg;
	const void *cgiArg2;
} HttpdBuiltInUrl;

const char *httpdCgiEx;  /* Magic for use in CgiArgs to interpret CgiArgs2 as HttpdCgiExArg */

typedef struct {
	void (*headerCb)(HttpdConnData *connData);
	const char *mimetype;
	const char *basepath;
} HttpdCgiExArg;

void httpdRedirect(HttpdConnData *conn, const char *newUrl);

// Decode a percent-encoded value.
// Takes the valLen bytes stored in val, and converts it into at most retLen bytes that
// are stored in the ret buffer. ret is always null terminated.
// @return True if decoding fit into the ret buffer, false if not
bool httpdUrlDecode(const char *val, int valLen, char *ret, int retLen, int* bytesWritten);

int httpdFindArg(const char *line, const char *arg, char *buff, int buffLen);

typedef enum
{
	HTTPD_FLAG_NONE = (1 << 0),
	HTTPD_FLAG_SSL = (1 << 1)
} HttpdFlags;

typedef enum
{
	InitializationSuccess,
	InitializationFailure
} HttpdInitStatus;

/** Common elements to the core server code */
typedef struct HttpdInstance
{
	const HttpdBuiltInUrl *builtInUrls;

	int maxConnections;
} HttpdInstance;

typedef enum
{
	CallbackSuccess,
	CallbackErrorOutOfConnections,
	CallbackErrorCannotFindConnection,
	CallbackErrorMemory,
	CallbackError
} CallbackStatus;

const char *httpdGetMimetype(const char *url);
void httpdSetTransferMode(HttpdConnData *conn, TransferModes mode);
void httpdStartResponse(HttpdConnData *conn, int code);
void httpdHeader(HttpdConnData *conn, const char *field, const char *val);
void httpdEndHeaders(HttpdConnData *conn);

/**
 * Get the value of a certain header in the HTTP client head
 * Returns true when found, false when not found.
 *
 * NOTE: 'ret' will be null terminated
 *
 * @param retLen is the number of bytes available in 'ret'
 */
bool httpdGetHeader(HttpdConnData *conn, const char *header, char *ret, int retLen);

int httpdSend(HttpdConnData *conn, const char *data, int len);
int httpdSend_js(HttpdConnData *conn, const char *data, int len);
int httpdSend_html(HttpdConnData *conn, const char *data, int len);
void httpdFlushSendBuffer(HttpdInstance *pInstance, HttpdConnData *conn);
CallbackStatus httpdContinue(HttpdInstance *pInstance, HttpdConnData *conn);
CallbackStatus httpdConnSendStart(HttpdInstance *pInstance, HttpdConnData *conn);
void httpdConnSendFinish(HttpdInstance *pInstance, HttpdConnData *conn);
void httpdAddCacheHeaders(HttpdConnData *connData, const char *mime);

//Platform dependent code should call these.
CallbackStatus httpdSentCb(HttpdInstance *pInstance, HttpdConnData *pConn);
CallbackStatus httpdRecvCb(HttpdInstance *pInstance, HttpdConnData *pConn, char *data, unsigned short len);
CallbackStatus httpdDisconCb(HttpdInstance *pInstance, HttpdConnData *pConn);

/** NOTE: httpdConnectCb() cannot fail */
void httpdConnectCb(HttpdInstance *pInstance, HttpdConnData *pConn);

#define esp_container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#ifdef CONFIG_ESPHTTPD_SHUTDOWN_SUPPORT
void httpdShutdown(HttpdInstance *pInstance);
#endif

#ifdef __cplusplus
}
#endif

#endif
