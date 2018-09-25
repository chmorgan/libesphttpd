/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Http server - core routines
*/

/* Copyright 2017 Jeroen Domburg <git@j0h.nl> */
/* Copyright 2017 Chris Morgan <chmorgan@gmail.com> */


#ifdef linux
#include <libesphttpd/linux.h>
#else
#include <libesphttpd/esp.h>
#endif

#include <strings.h>

#include "libesphttpd/httpd.h"
#include "httpd-platform.h"

#include "esp_log.h"

const static char* TAG = "httpd";

//Flags
#define HFL_HTTP11 (1<<0)
#define HFL_CHUNKED (1<<1)
#define HFL_SENDINGBODY (1<<2)
#define HFL_DISCONAFTERSENT (1<<3)
#define HFL_NOCONNECTIONSTR (1<<4)


//Struct to keep extension->mime data in
typedef struct {
    const char *ext;
    const char *mimetype;
} MimeMap;


//#define RSTR(a) ((const char)(a))

//The mappings from file extensions to mime types. If you need an extra mime type,
//add it here.
static const ICACHE_RODATA_ATTR MimeMap mimeTypes[]={
    {"htm", "text/html"},
    {"html", "text/html"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"txt", "text/plain"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"png", "image/png"},
    {"svg", "image/svg+xml"},
    {"xml", "text/xml"},
    {"json", "application/json"},
    {NULL, "text/html"}, //default value
};

//Returns a static char* to a mime type for a given url to a file.
const char ICACHE_FLASH_ATTR *httpdGetMimetype(const char *url) {
    char *urlp = (char*)url;
    int i=0;
    //Go find the extension
    const char *ext=urlp+(strlen(urlp)-1);
    while (ext!=urlp && *ext!='.') ext--;
    if (*ext=='.') ext++;

    while (mimeTypes[i].ext!=NULL && strcasecmp(ext, mimeTypes[i].ext)!=0) i++;
    return mimeTypes[i].mimetype;
}

/**
* Add sensible cache control headers to avoid needless asset reloading
*
* @param connData
* @param mime - mime type string
*/
void httpdAddCacheHeaders(HttpdConnData *connData, const char *mime)
{
    if (strcmp(mime, "text/html") == 0) return;
    if (strcmp(mime, "text/plain") == 0) return;
    if (strcmp(mime, "text/csv") == 0) return;
    if (strcmp(mime, "application/json") == 0) return;

    httpdHeader(connData, "Cache-Control", "max-age=7200, public, must-revalidate");
}

//Retires a connection for re-use
static void ICACHE_FLASH_ATTR httpdRetireConn(HttpdInstance *pInstance, HttpdConnData *conn) {
#ifdef CONFIG_ESPHTTPD_BACKLOG_SUPPORT
    if (conn->priv.sendBacklog!=NULL) {
        HttpSendBacklogItem *i, *j;
        i=conn->priv.sendBacklog;
        do {
            j=i;
            i=i->next;
            free(j);
        } while (i!=NULL);
    }
#endif

    if (conn->post.buff)
    {
        free(conn->post.buff);
        conn->post.buff = NULL;
    }
}

//Stupid li'l helper function that returns the value of a hex char.
static int ICACHE_FLASH_ATTR  httpdHexVal(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='A' && c<='F') return c-'A'+10;
    if (c>='a' && c<='f') return c-'a'+10;
    return 0;
}

bool ICACHE_FLASH_ATTR httpdUrlDecode(const char *val, int valLen, char *ret, int retLen, int* bytesWritten) {
    int s=0; // index of theread position in val
    int d=0; // index of the write position in 'ret'
    int esced=0, escVal=0;
    // d loops for (retLen - 1) to ensure there is space for the null terminator
    while (s<valLen && d < (retLen - 1)) {
        if (esced==1)  {
            escVal=httpdHexVal(val[s])<<4;
            esced=2;
        } else if (esced==2) {
            escVal+=httpdHexVal(val[s]);
            ret[d++]=escVal;
            esced=0;
        } else if (val[s]=='%') {
            esced=1;
        } else if (val[s]=='+') {
            ret[d++]=' ';
        } else {
            ret[d++]=val[s];
        }
        s++;
    }

    ret[d++] = 0;
    *bytesWritten = d;

    // if s == valLen then we processed all of the input bytes
    return (s == valLen) ? true : false;
}

//Find a specific arg in a string of get- or post-data.
//Line is the string of post/get-data, arg is the name of the value to find. The
//zero-terminated result is written in buff, with at most buffLen bytes used. The
//function returns the length of the result, or -1 if the value wasn't found. The
//returned string will be urldecoded already.
int ICACHE_FLASH_ATTR httpdFindArg(const char *line, const char *arg, char *buff, int buffLen) {
    const char *p, *e;
    if (line==NULL) return -1;
    const int arglen = strlen(arg);
    p=line;
    while(p!=NULL && *p!='\n' && *p!='\r' && *p!=0) {
        if (strncmp(p, arg, arglen)==0 && p[arglen]=='=') {
            p+=arglen+1; //move p to start of value
            e=(char*)strstr(p, "&");
            if (e==NULL) e=p+strlen(p);
            int bytesWritten;
            if(!httpdUrlDecode(p, (e-p), buff, buffLen, &bytesWritten))
            {
                //TODO: proper error return through this code path
                ESP_LOGE(TAG, "out of space storing url");
            }
            return bytesWritten;
        }
        p=(char*)strstr(p, "&");
        if (p!=NULL) p+=1;
    }
    ESP_LOGD(TAG, "Finding %s in %s: Not found", arg, line);
    return -1; //not found
}

bool ICACHE_FLASH_ATTR httpdGetHeader(HttpdConnData *conn, const char *header, char *ret, int retLen) {
    bool retval = false;

    char *p=conn->priv.head;
    p=p+strlen(p)+1; //skip GET/POST part
    p=p+strlen(p)+1; //skip HTTP part
    while (p<(conn->priv.head+conn->priv.headPos)) {
        while(*p<=32 && *p!=0) p++; //skip crap at start
        //See if this is the header
        if (strncasecmp(p, header, strlen(header))==0 && p[strlen(header)]==':') {
            //Skip 'key:' bit of header line
            p=p+strlen(header)+1;
            //Skip past spaces after the colon
            while(*p==' ') p++;
            //Copy from p to end
            // retLen check preserves one byte in ret so we can null terminate
            while (*p!=0 && *p!='\r' && *p!='\n' && retLen>1) {
                *ret++=*p++;
                retLen--;
            }
            //Zero-terminate string
            *ret=0;
            retval = true;
        }
        p+=strlen(p)+1; //Skip past end of string and \0 terminator
    }

    return retval;
}

void ICACHE_FLASH_ATTR httpdSetTransferMode(HttpdConnData *conn, TransferModes mode) {
    if (mode==HTTPD_TRANSFER_CLOSE) {
        conn->priv.flags&=~HFL_CHUNKED;
        conn->priv.flags&=~HFL_NOCONNECTIONSTR;
    } else if (mode==HTTPD_TRANSFER_CHUNKED) {
        conn->priv.flags|=HFL_CHUNKED;
        conn->priv.flags&=~HFL_NOCONNECTIONSTR;
    } else if (mode==HTTPD_TRANSFER_NONE) {
        conn->priv.flags&=~HFL_CHUNKED;
        conn->priv.flags|=HFL_NOCONNECTIONSTR;
    }
}

//Start the response headers.
void ICACHE_FLASH_ATTR httpdStartResponse(HttpdConnData *conn, int code) {
    char buff[128];
    int l;
    const char *connStr="Connection: close\r\n";
    if (conn->priv.flags&HFL_CHUNKED) connStr="Transfer-Encoding: chunked\r\n";
    if (conn->priv.flags&HFL_NOCONNECTIONSTR) connStr="";
    l=snprintf(buff, sizeof(buff), "HTTP/1.%d %d OK\r\nServer: esp-httpd/"HTTPDVER"\r\n%s",
                (conn->priv.flags&HFL_HTTP11)?1:0,
                code,
                connStr);
    if(l >= sizeof(buff))
    {
        ESP_LOGE(TAG, "buff[%zu] too small", sizeof(buff));
    }
    httpdSend(conn, buff, l);

#ifdef CONFIG_ESPHTTPD_CORS_SUPPORT
    // CORS headers
    httpdSend(conn, "Access-Control-Allow-Origin: *\r\n", -1);
    httpdSend(conn, "Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS\r\n", -1);
#endif
}

//Send a http header.
void ICACHE_FLASH_ATTR httpdHeader(HttpdConnData *conn, const char *field, const char *val) {
    httpdSend(conn, field, -1);
    httpdSend(conn, ": ", -1);
    httpdSend(conn, val, -1);
    httpdSend(conn, "\r\n", -1);
}

//Finish the headers.
void ICACHE_FLASH_ATTR httpdEndHeaders(HttpdConnData *conn) {
    httpdSend(conn, "\r\n", -1);
    conn->priv.flags|=HFL_SENDINGBODY;
}

//Redirect to the given URL.
void ICACHE_FLASH_ATTR httpdRedirect(HttpdConnData *conn, const char *newUrl) {
    httpdStartResponse(conn, 302);
    httpdHeader(conn, "Location", newUrl);
    httpdEndHeaders(conn);
    httpdSend(conn, "Moved to ", -1);
    httpdSend(conn, newUrl, -1);
}

//Used to spit out a 404 error
static CgiStatus ICACHE_FLASH_ATTR cgiNotFound(HttpdConnData *connData) {
    if (connData->isConnectionClosed) return HTTPD_CGI_DONE;
    httpdStartResponse(connData, 404);
    httpdEndHeaders(connData);
    httpdSend(connData, "404 File not found.", -1);
    return HTTPD_CGI_DONE;
}

static const char* CHUNK_SIZE_TEXT = "0000\r\n";
static const int CHUNK_SIZE_TEXT_LEN = 6; // number of characters in CHUNK_SIZE_TEXT

//Add data to the send buffer. len is the length of the data. If len is -1
//the data is seen as a C-string.
//Returns 1 for success, 0 for out-of-memory.
int ICACHE_FLASH_ATTR httpdSend(HttpdConnData *conn, const char *data, int len) {
    if (len<0) len=strlen(data);
    if (len==0) return 0;
    if (conn->priv.flags&HFL_CHUNKED && conn->priv.flags&HFL_SENDINGBODY && conn->priv.chunkHdr==NULL)
    {
        if (conn->priv.sendBuffLen+len+CHUNK_SIZE_TEXT_LEN > HTTPD_MAX_SENDBUFF_LEN) return 0;

        // Establish start of chunk
        // Use a chunk length placeholder of 4 characters
        conn->priv.chunkHdr = &conn->priv.sendBuff[conn->priv.sendBuffLen];
        strcpy(conn->priv.chunkHdr, CHUNK_SIZE_TEXT);
        conn->priv.sendBuffLen+=CHUNK_SIZE_TEXT_LEN;
        assert(conn->priv.sendBuffLen <= HTTPD_MAX_SENDBUFF_LEN);
    }
    if (conn->priv.sendBuffLen+len > HTTPD_MAX_SENDBUFF_LEN) return 0;
    memcpy(conn->priv.sendBuff+conn->priv.sendBuffLen, data, len);
    conn->priv.sendBuffLen+=len;
    assert(conn->priv.sendBuffLen <= HTTPD_MAX_SENDBUFF_LEN);
    return 1;
}

static char ICACHE_FLASH_ATTR httpdHexNibble(int val)
{
    val&=0xf;
    if (val<10) return '0'+val;
    return 'A'+(val-10);
}

#define httpdSend_orDie(conn, data, len) do { if (!httpdSend((conn), (data), (len))) return false; } while (0)

/* encode for HTML. returns 0 or 1 - 1 = success */
int ICACHE_FLASH_ATTR httpdSend_html(HttpdConnData *conn, const char *data, int len)
{
    int start = 0, end = 0;
    char c;
    if (len < 0) len = (int) strlen(data);
    if (len==0) return 0;

    for (end = 0; end < len; end++)
    {
        c = data[end];
        if (c == 0) {
            // we found EOS
            break;
        }

        if (c == '"' || c == '\'' || c == '<' || c == '>')
        {
            if (start < end) httpdSend_orDie(conn, data + start, end - start);
            start = end + 1;
        }

        if (c == '"') httpdSend_orDie(conn, "&#34;", 5);
        else if (c == '\'') httpdSend_orDie(conn, "&#39;", 5);
        else if (c == '<') httpdSend_orDie(conn, "&lt;", 4);
        else if (c == '>') httpdSend_orDie(conn, "&gt;", 4);
    }

    if (start < end) httpdSend_orDie(conn, data + start, end - start);
    return 1;
}

/* encode for JS. returns 0 or 1 - 1 = success */
int ICACHE_FLASH_ATTR httpdSend_js(HttpdConnData *conn, const char *data, int len)
{
    int start = 0, end = 0;
    char c;
    if (len < 0) len = (int) strlen(data);
    if (len==0) return 0;

    for (end = 0; end < len; end++)
    {
        c = data[end];
        if (c == 0)
        {
            // we found EOS
            break;
        }

        if (c == '"' || c == '\\' || c == '\'' || c == '<' || c == '>' || c == '\n' || c == '\r') {
            if (start < end) httpdSend_orDie(conn, data + start, end - start);
            start = end + 1;
        }

        if (c == '"') httpdSend_orDie(conn, "\\\"", 2);
        else if (c == '\'') httpdSend_orDie(conn, "\\'", 2);
        else if (c == '\\') httpdSend_orDie(conn, "\\\\", 2);
        else if (c == '<') httpdSend_orDie(conn, "\\u003C", 6);
        else if (c == '>') httpdSend_orDie(conn, "\\u003E", 6);
        else if (c == '\n') httpdSend_orDie(conn, "\\n", 2);
        else if (c == '\r') httpdSend_orDie(conn, "\\r", 2);
    }

    if (start < end) httpdSend_orDie(conn, data + start, end - start);
    return 1;
}

//Function to send any data in conn->priv.sendBuff. Do not use in CGIs unless you know what you
//are doing! Also, if you do set conn->cgi to NULL to indicate the connection is closed, do it BEFORE
//calling this.
void ICACHE_FLASH_ATTR httpdFlushSendBuffer(HttpdInstance *pInstance, HttpdConnData *conn)
{
    int r, len;
    if (conn->priv.chunkHdr!=NULL) {
        //We're sending chunked data, and the chunk needs fixing up.
        //Finish chunk with cr/lf
        httpdSend(conn, "\r\n", 2);
        //Calculate length of chunk
        // +2 is to remove the two characters written above via httpdSend(), those
        // bytes aren't counted in the chunk length
        len=((&conn->priv.sendBuff[conn->priv.sendBuffLen])-conn->priv.chunkHdr) - (CHUNK_SIZE_TEXT_LEN + 2);
        //Fix up chunk header to correct value
        conn->priv.chunkHdr[0]=httpdHexNibble(len>>12);
        conn->priv.chunkHdr[1]=httpdHexNibble(len>>8);
        conn->priv.chunkHdr[2]=httpdHexNibble(len>>4);
        conn->priv.chunkHdr[3]=httpdHexNibble(len>>0);
        //Reset chunk hdr for next call
        conn->priv.chunkHdr=NULL;
    }
    if (conn->priv.flags&HFL_CHUNKED && conn->priv.flags&HFL_SENDINGBODY && conn->cgi==NULL) {
        if(conn->priv.sendBuffLen + 5 <= HTTPD_MAX_SENDBUFF_LEN)
        {
            //Connection finished sending whatever needs to be sent. Add NULL chunk to indicate this.
            strcpy(&conn->priv.sendBuff[conn->priv.sendBuffLen], "0\r\n\r\n");
            conn->priv.sendBuffLen+=5;
            assert(conn->priv.sendBuffLen <= HTTPD_MAX_SENDBUFF_LEN);
        } else
        {
            ESP_LOGE(TAG, "sendBuff full");
        }
    }
    if (conn->priv.sendBuffLen!=0)
    {
        r = httpdPlatSendData(pInstance, conn, conn->priv.sendBuff, conn->priv.sendBuffLen);
        if (r != conn->priv.sendBuffLen) {
#ifdef CONFIG_ESPHTTPD_BACKLOG_SUPPORT
            //Can't send this for some reason. Dump packet in backlog, we can send it later.
            if (conn->priv.sendBacklogSize+conn->priv.sendBuffLen>HTTPD_MAX_BACKLOG_SIZE) {
                ESP_LOGE(TAG, "Backlog: Exceeded max backlog size, dropped %d bytes", conn->priv.sendBuffLen);
                conn->priv.sendBuffLen=0;
                return;
            }
            HttpSendBacklogItem *i=malloc(sizeof(HttpSendBacklogItem)+conn->priv.sendBuffLen);
            if (i==NULL) {
                ESP_LOGE(TAG, "Backlog: malloc failed");
                return;
            }
            memcpy(i->data, conn->priv.sendBuff, conn->priv.sendBuffLen);
            i->len=conn->priv.sendBuffLen;
            i->next=NULL;
            if (conn->priv.sendBacklog==NULL) {
                conn->priv.sendBacklog=i;
            } else {
                HttpSendBacklogItem *e=conn->priv.sendBacklog;
                while (e->next!=NULL) e=e->next;
                e->next=i;
            }
            conn->priv.sendBacklogSize+=conn->priv.sendBuffLen;
#else
            ESP_LOGE(TAG, "send buf tried to write %d bytes, wrote %d", conn->priv.sendBuffLen, r);
#endif
        }
        conn->priv.sendBuffLen=0;
    }
}

void ICACHE_FLASH_ATTR httpdCgiIsDone(HttpdInstance *pInstance, HttpdConnData *conn) {
    conn->cgi=NULL; //no need to call this anymore

    if (conn->priv.flags&HFL_CHUNKED)
    {
        ESP_LOGD(TAG, "cleaning up");
        httpdFlushSendBuffer(pInstance, conn);
        //Note: Do not clean up sendBacklog, it may still contain data at this point.
        conn->priv.headPos=0;
        conn->post.len=-1;
        conn->priv.flags=0;
        if (conn->post.buff) free(conn->post.buff);
        conn->post.buff=NULL;
        conn->post.buffLen=0;
        conn->post.received=0;
        conn->hostName=NULL;
    } else {
        //Cannot re-use this connection. Mark to get it killed after all data is sent.
        conn->priv.flags|=HFL_DISCONAFTERSENT;
    }
}

//Callback called when the data on a socket has been successfully
//sent.
CallbackStatus ICACHE_FLASH_ATTR httpdSentCb(HttpdInstance *pInstance, HttpdConnData *pConn) {
    return httpdContinue(pInstance, pConn);
}

//Can be called after a CGI function has returned HTTPD_CGI_MORE to
//resume handling an open connection asynchronously
CallbackStatus ICACHE_FLASH_ATTR httpdContinue(HttpdInstance *pInstance, HttpdConnData * conn) {
    int r;
    httpdPlatLock(pInstance);
    CallbackStatus status = CallbackSuccess;

#ifdef CONFIG_ESPHTTPD_BACKLOG_SUPPORT
    if (conn->priv.sendBacklog!=NULL) {
        //We have some backlog to send first.
        HttpSendBacklogItem *next=conn->priv.sendBacklog->next;
        int bytesWritten = httpdPlatSendData(pInstance, conn->conn, conn->priv.sendBacklog->data, conn->priv.sendBacklog->len);
        if(bytesWritten != conn->priv.sendBacklog->len)
        {
            ESP_LOGE(TAG, "tried to write %d bytes, wrote %d", conn->priv.sendBacklog->len, bytesWritten);
        }
        conn->priv.sendBacklogSize-=conn->priv.sendBacklog->len;
        free(conn->priv.sendBacklog);
        conn->priv.sendBacklog=next;
        httpdPlatUnlock(pInstance);
        return CallbackSuccess;
    }
#endif

    if (conn->priv.flags & HFL_DISCONAFTERSENT) { //Marked for destruction?
        ESP_LOGD(TAG, "closing");
        httpdPlatDisconnect(conn);
        status = CallbackSuccess;
        // NOTE: No need to call httpdFlushSendBuffer
    } else
    {
        //If we don't have a CGI function, there's nothing to do but wait for something from the client.
        if (conn->cgi == NULL)
        {
            status = CallbackSuccess;
        } else
        {
            conn->priv.sendBuffLen = 0;

            r = conn->cgi(conn); //Execute cgi fn.

            if (r==HTTPD_CGI_DONE)
            {
                // No special action for HTTPD_CGI_DONE
            } else if(r==HTTPD_CGI_NOTFOUND || r==HTTPD_CGI_AUTHENTICATED)
            {
                ESP_LOGE(TAG, "CGI fn returned %d", r);
            }

            if((r == HTTPD_CGI_DONE) || (r == HTTPD_CGI_NOTFOUND) ||
                (r == HTTPD_CGI_AUTHENTICATED))
            {
                httpdCgiIsDone(pInstance, conn);
            }

            httpdFlushSendBuffer(pInstance, conn);
        }
    }

    httpdPlatUnlock(pInstance);
    return status;
}

//This is called when the headers have been received and the connection is ready to send
//the result headers and data.
//We need to find the CGI function to call, call it, and dependent on what it returns either
//find the next cgi function, wait till the cgi data is sent or close up the connection.
static void ICACHE_FLASH_ATTR httpdProcessRequest(HttpdInstance *pInstance, HttpdConnData *conn) {
    int r;
    int i=0;
    if (conn->url==NULL)
    {
        ESP_LOGE(TAG, "url = NULL");
        return; //Shouldn't happen
    }

#ifdef CONFIG_ESPHTTPD_CORS_SUPPORT
    // CORS preflight, allow the token we received before
    if (conn->requestType == HTTPD_METHOD_OPTIONS)
    {
        httpdStartResponse(conn, 200);
        httpdHeader(conn, "Access-Control-Allow-Headers", conn->priv.corsToken);
        httpdEndHeaders(conn);
        httpdCgiIsDone(pInstance, conn);

        ESP_LOGD(TAG, "CORS preflight resp sent");
        return;
    }
#endif

    //See if we can find a CGI that's happy to handle the request.
    while (1)
    {
        //Look up URL in the built-in URL table.
        while (pInstance->builtInUrls[i].url!=NULL) {
            const HttpdBuiltInUrl *pUrl = &(pInstance->builtInUrls[i]);
            bool match = false;
            const char* route = pUrl->url;

            //See if there's a literal match
            if (strcmp(route, conn->url)==0)
            {
                match = true;
            } else if(route[strlen(route)-1]=='*' &&
                        strncmp(route, conn->url, strlen(route)-1)==0)
            {
                // See if there's a wildcard match, if the route entry ends in '*'
                // and everything up to the '*' is a match
                match = true;
            }

            if (match) {
                ESP_LOGD(TAG, "Is url index %d", i);
                conn->cgiData=NULL;
                conn->cgi=pUrl->cgiCb;
                conn->cgiArg=pUrl->cgiArg;
                conn->cgiArg2=pUrl->cgiArg2;
                break;
            }
            i++;
        }
        if (pInstance->builtInUrls[i].url==NULL) {
            //Drat, we're at the end of the URL table. This usually shouldn't happen. Well, just
            //generate a built-in 404 to handle this.
            ESP_LOGD(TAG, "%s not found. 404", conn->url);
            conn->cgi=cgiNotFound;
        }

        //Okay, we have a CGI function that matches the URL. See if it wants to handle the
        //particular URL we're supposed to handle.
        r=conn->cgi(conn);
        if (r==HTTPD_CGI_MORE) {
            //Yep, it's happy to do so and has more data to send.
            if (conn->recvHdl) {
                //Seems the CGI is planning to do some long-term communications with the socket.
                //Disable the timeout on it, so we won't run into that.
                httpdPlatDisableTimeout(conn);
            }
            httpdFlushSendBuffer(pInstance, conn);
            break;
        } else if (r==HTTPD_CGI_DONE) {
            //Yep, it's happy to do so and already is done sending data.
            httpdCgiIsDone(pInstance, conn);
            break;
        } else if (r==HTTPD_CGI_NOTFOUND || r==HTTPD_CGI_AUTHENTICATED) {
            //URL doesn't want to handle the request: either the data isn't found or there's no
            //need to generate a login screen.
            i++; //look at next url the next iteration of the loop.
        }
    }
}

//Parse a line of header data and modify the connection data accordingly.
static CallbackStatus ICACHE_FLASH_ATTR httpdParseHeader(char *h, HttpdConnData *conn) {
    int i;
    char firstLine=0;
    CallbackStatus status = CallbackSuccess;

    if (strncmp(h, "GET ", 4)==0) {
        conn->requestType = HTTPD_METHOD_GET;
        firstLine=1;
    } else if (strncasecmp(h, "Host:", 5)==0) {
        i=5;
        while (h[i]==' ') i++;
        conn->hostName=&h[i];
    } else if (strncmp(h, "POST ", 5)==0) {
        conn->requestType = HTTPD_METHOD_POST;
        firstLine=1;
    } else if (strncmp(h, "OPTIONS ", 8)==0) {
        conn->requestType = HTTPD_METHOD_OPTIONS;
        firstLine=1;
    } else if (strncmp(h, "PUT ", 4)==0) {
        conn->requestType = HTTPD_METHOD_PUT;
        firstLine=1;
    } else if (strncmp(h, "PATCH ", 6)==0) {
        conn->requestType = HTTPD_METHOD_PATCH;
        firstLine=1;
    } else if (strncmp(h, "DELETE ", 7)==0) {
        conn->requestType = HTTPD_METHOD_DELETE;
        firstLine=1;
    }
    if (firstLine) {
        char *e;

        //Skip past the space after POST/GET
        i=0;
        while (h[i]!=' ') i++;
        conn->url=h+i+1;

        //Figure out end of url.
        e=(char*)strstr(conn->url, " ");
        if (e==NULL) return CallbackError;
        *e=0; //terminate url part
        e++; //Skip to protocol indicator
        while (*e==' ') e++; //Skip spaces.
        //If HTTP/1.1, note that and set chunked encoding
        if (strcasecmp(e, "HTTP/1.1")==0) conn->priv.flags|=HFL_HTTP11|HFL_CHUNKED;

        ESP_LOGD(TAG, "URL = %s", conn->url);
        //Parse out the URL part before the GET parameters.
        conn->getArgs=(char*)strstr(conn->url, "?");
        if (conn->getArgs!=0) {
            *conn->getArgs=0;
            conn->getArgs++;
            ESP_LOGD(TAG, "GET args = %s", conn->getArgs);
        } else {
            conn->getArgs=NULL;
        }

#ifdef CONFIG_ESPHTTPD_SANITIZE_URLS
        // Remove multiple repeated slashes in URL path.
        while((e = strstr(conn->url, "//")) != NULL){
            // Move string starting at second slash one place to the left.
            // Use strlen() on the first slash so the '\0' will be copied too.
            ESP_LOGD(TAG, "Cleaning up URL path: %s", conn->url);
            memmove(e, e + 1, strlen(e));
            ESP_LOGD(TAG, "Cleaned URL path: %s", conn->url);
        }
#endif // CONFIG_ESPHTTPD_SANITIZE_URLS
    } else if (strncasecmp(h, "Connection:", 11)==0) {
        i=11;
        //Skip trailing spaces
        while (h[i]==' ') i++;
        if (strncmp(&h[i], "close", 5)==0) conn->priv.flags&=~HFL_CHUNKED; //Don't use chunked conn
    } else if (strncasecmp(h, "Content-Length:", 15)==0) {
        i=15;
        //Skip trailing spaces
        while (h[i]==' ') i++;
        //Get POST data length
        conn->post.len=atoi(h+i);

        // Allocate the buffer
        if (conn->post.len > HTTPD_MAX_POST_LEN) {
            // we'll stream this in in chunks
            conn->post.buffSize = HTTPD_MAX_POST_LEN;
        } else {
            conn->post.buffSize = conn->post.len;
        }

        ESP_LOGD(TAG, "Mallocced buffer for %d + 1 bytes of post data", conn->post.buffSize);
        int bufferSize = conn->post.buffSize + 1;
        conn->post.buff=(char*)malloc(bufferSize);
        if (conn->post.buff==NULL) {
            ESP_LOGE(TAG, "malloc failed %d bytes", bufferSize);
            status = CallbackErrorMemory;
        } else
        {
            conn->post.buffLen=0;
        }
    } else if (strncasecmp(h, "Content-Type: ", 14)==0) {
        if (strstr(h, "multipart/form-data")) {
            // It's multipart form data so let's pull out the boundary
            // TODO: implement multipart support in the server
            char *b;
            const char* boundaryToken = "boundary=";
            if ((b = strstr(h, boundaryToken)) != NULL) {
                conn->post.multipartBoundary = b + strlen(boundaryToken);
                ESP_LOGD(TAG, "boundary = %s", conn->post.multipartBoundary);
            }
        }
    }
#ifdef CONFIG_ESPHTTPD_CORS_SUPPORT
    else if (strncmp(h, "Access-Control-Request-Headers: ", 32)==0) {
        // CORS token must be repeated in the response, copy it into
        // the connection token storage
        ESP_LOGD(TAG, "CORS preflight request");

        strncpy(conn->priv.corsToken, h+strlen("Access-Control-Request-Headers: "), MAX_CORS_TOKEN_LEN);

        // ensure null termination of the token
        conn->priv.corsToken[MAX_CORS_TOKEN_LEN-1] = 0;
    }
#endif

    return status;
}

//Make a connection 'live' so we can do all the things a cgi can do to it.
//ToDo: Also make httpdRecvCb/httpdContinue use these?
CallbackStatus ICACHE_FLASH_ATTR httpdConnSendStart(HttpdInstance *pInstance, HttpdConnData *conn) {
    CallbackStatus status;
    httpdPlatLock(pInstance);

    conn->priv.sendBuffLen=0;
    status = CallbackSuccess;

    return status;
}

//Finish the live-ness of a connection. Always call this after httpdConnStart
void ICACHE_FLASH_ATTR httpdConnSendFinish(HttpdInstance *pInstance, HttpdConnData *conn) {
    httpdFlushSendBuffer(pInstance, conn);
    httpdPlatUnlock(pInstance);
}

//Callback called when there's data available on a socket.
CallbackStatus ICACHE_FLASH_ATTR httpdRecvCb(HttpdInstance *pInstance, HttpdConnData *conn, char *data, unsigned short len) {
    int x, r;
    char *p, *e;
    CallbackStatus status = CallbackSuccess;
    httpdPlatLock(pInstance);

    conn->priv.sendBuffLen=0;
#ifdef CONFIG_ESPHTTPD_CORS_SUPPORT
    conn->priv.corsToken[0] = 0;
#endif

    //This is slightly evil/dirty: we abuse conn->post.len as a state variable for where in the http communications we are:
    //<0 (-1): Post len unknown because we're still receiving headers
    //==0: No post data
    //>0: Need to receive post data
    //ToDo: See if we can use something more elegant for this.

    for (x=0; x<len; x++)
    {
        if (conn->post.len<0) // This byte is a header byte
        {
            if (data[x]=='\n')
            {
                if(conn->priv.headPos < HTTPD_MAX_HEAD_LEN-1)
                {
                    //Compatibility with clients that send \n only: fake a \r in front of this.
                    if (conn->priv.headPos!=0 && conn->priv.head[conn->priv.headPos-1]!='\r') {
                        conn->priv.head[conn->priv.headPos++]='\r';
                    }
                } else
                {
                    ESP_LOGE(TAG, "adding newline request too long");
                }
            }

            //ToDo: return http error code 431 (request header too long) if this happens
            if (conn->priv.headPos < HTTPD_MAX_HEAD_LEN-1)
            {
                conn->priv.head[conn->priv.headPos++]=data[x];
            } else
            {
                ESP_LOGE(TAG, "request too long!");
            }

            // always null terminate
            conn->priv.head[conn->priv.headPos]=0;

            //Scan for /r/n/r/n. Receiving this indicate the headers end.
            if (data[x]=='\n' && (char *)strstr(conn->priv.head, "\r\n\r\n")!=NULL) {
                //Indicate we're done with the headers.
                conn->post.len=0;
                //Reset url data
                conn->url=NULL;
                //Iterate over all received headers and parse them.
                p=conn->priv.head;
                while(p<(&conn->priv.head[conn->priv.headPos-4])) {
                    e=(char *)strstr(p, "\r\n"); //Find end of header line
                    if (e==NULL) break;			//Shouldn't happen.
                    e[0]=0;						//Zero-terminate header
                    httpdParseHeader(p, conn);	//and parse it.
                    p=e+2;						//Skip /r/n (now /0/n)
                }
                //If we don't need to receive post data, we can send the response now.
                if (conn->post.len==0) {
                    httpdProcessRequest(pInstance, conn);
                }
            }
        } else if (conn->post.len!=0) {
            //This byte is a POST byte.
            conn->post.buff[conn->post.buffLen++]=data[x];
            conn->post.received++;
            conn->hostName=NULL;
            if (conn->post.buffLen >= conn->post.buffSize || conn->post.received == conn->post.len) {
                //Received a chunk of post data
                conn->post.buff[conn->post.buffLen]=0; //zero-terminate, in case the cgi handler knows it can use strings
                //Process the data
                if (conn->cgi) {
                    r=conn->cgi(conn);
                    if (r==HTTPD_CGI_DONE) {
                        httpdCgiIsDone(pInstance, conn);
                    }
                } else {
                    //No CGI fn set yet: probably first call. Allow httpdProcessRequest to choose CGI and
                    //call it the first time.
                    httpdProcessRequest(pInstance, conn);
                }
                conn->post.buffLen = 0;
            }
        } else {
            //Let cgi handle data if it registered a recvHdl callback. If not, ignore.
            if (conn->recvHdl) {
                r=conn->recvHdl(pInstance, conn, data+x, len-x);
                if (r==HTTPD_CGI_DONE) {
                    ESP_LOGD(TAG, "Recvhdl returned DONE");
                    httpdCgiIsDone(pInstance, conn);
                    //We assume the recvhdlr has sent something; we'll kill the sock in the sent callback.
                }
                break; //ignore rest of data, recvhdl has parsed it.
            } else {
                ESP_LOGE(TAG, "Unexpected data from client. %s", data);
                status = CallbackError;
            }
        }
    }
    httpdFlushSendBuffer(pInstance, conn);
    httpdPlatUnlock(pInstance);

    return status;
}

//The platform layer should ALWAYS call this function, regardless if the connection is closed by the server
//or by the client.
CallbackStatus ICACHE_FLASH_ATTR httpdDisconCb(HttpdInstance *pInstance, HttpdConnData *pConn) {
    httpdPlatLock(pInstance);

    ESP_LOGD(TAG, "Socket closed");
    pConn->isConnectionClosed = true;
    if (pConn->cgi) pConn->cgi(pConn); //Execute cgi fn if needed
    httpdRetireConn(pInstance, pConn);
    httpdPlatUnlock(pInstance);

    return CallbackSuccess;
}


void ICACHE_FLASH_ATTR httpdConnectCb(HttpdInstance *pInstance, HttpdConnData *pConn) {
    httpdPlatLock(pInstance);

    memset(pConn, 0, sizeof(HttpdConnData));
    pConn->post.len=-1;

    httpdPlatUnlock(pInstance);
}

#ifdef CONFIG_ESPHTTPD_SHUTDOWN_SUPPORT
void httpdShutdown(HttpdInstance *pInstance)
{
    httpdPlatShutdown(pInstance);
}
#endif
