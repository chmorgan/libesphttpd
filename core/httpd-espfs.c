/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Connector to let httpd use the espfs filesystem to serve the files in it.
*/

#ifdef linux
#include <libesphttpd/linux.h>
#else
#include <libesphttpd/esp.h>  // for sdkconfig.h
#endif

#include <libesphttpd/httpd-espfs.h>

#ifdef CONFIG_ESPHTTPD_USE_ESPFS
#include "espfs.h"
#include "espfsformat.h"  // just for FLAG_GZIP
#include "esp_log.h"
const static char* TAG = "httpdespfs";

#define FILE_CHUNK_LEN    1024

// The static files marked with FLAG_GZIP are compressed and will be served with GZIP compression.
// If the client does not advertise that he accepts GZIP send following warning message (telnet users for e.g.)
static const char *gzipNonSupportedMessage = "HTTP/1.0 501 Not implemented\r\nServer: esp8266-httpd/"HTTPDVER"\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 52\r\n\r\nYour browser does not accept gzip-compressed data.\r\n";

/**
 * Try to open a file
 * @param path - path to the file, may end with slash
 * @param indexname - filename at the path
 * @return file pointer or NULL
 */
static EspFsFile *tryOpenIndex_do(const char *path, const char *indexname) {
	char fname[100];
	EspFsFile *retval;
	size_t url_len = strlen(path);
	size_t index_len = strlen(indexname);
	bool needSlash = false;

	// will we need to append a slash?
	if(path[url_len - 1] != '/') {
		url_len++;
		needSlash = true;
	}

	// do we have enough space to handle the input strings
	// -1 to leave space for a trailing null
	if((url_len + index_len) >= (sizeof(fname) - 1))
	{
		retval = NULL;
		ESP_LOGE(TAG, "fname too small");
	} else
	{
		strcpy(fname, path);

		// Append slash if missing
		if(needSlash)
		{
			strcat(fname, "/");
		}

		strcat(fname, indexname);

		// Try to open, returns NULL if failed
		retval = espFsOpen(fname);
	}

	return retval;
}

/**
 * Try to find index file on a path
 * @param path - directory
 * @return file pointer or NULL
 */
EspFsFile *tryOpenIndex(const char *path) {
	EspFsFile * file;
	// A dot in the filename probably means extension
	// no point in trying to look for index.
	if (strchr(path, '.') != NULL) return NULL;

	file = tryOpenIndex_do(path, "index.html");
	if (file != NULL) return file;

	file = tryOpenIndex_do(path, "index.htm");
	if (file != NULL) return file;

	file = tryOpenIndex_do(path, "index.tpl.html");
	if (file != NULL) return file;

	file = tryOpenIndex_do(path, "index.tpl");
	if (file != NULL) return file;

	return NULL; // failed to guess the right name
}

CgiStatus ICACHE_FLASH_ATTR
serveStaticFile(HttpdConnData *connData, const char* filepath) {
	EspFsFile *file=connData->cgiData;
	int len;
	char buff[FILE_CHUNK_LEN+1];
	char acceptEncodingBuffer[64];
	int isGzip;

	if (connData->isConnectionClosed) {
		//Connection closed. Clean up.
		espFsClose(file);
		return HTTPD_CGI_DONE;
	}

	// invalid call.
	if (filepath == NULL) {
		ESP_LOGE(TAG, "serveStaticFile called with NULL path");
		return HTTPD_CGI_NOTFOUND;
	}

	//First call to this cgi.
	if (file==NULL) {
		//First call to this cgi. Open the file so we can read it.
		file = espFsOpen(filepath);
		if (file == NULL) {
			// file not found

			// If this is a folder, look for index file
			file = tryOpenIndex(filepath);
			if (file == NULL) return HTTPD_CGI_NOTFOUND;
		}

		// The gzip checking code is intentionally without #ifdefs because checking
		// for FLAG_GZIP (which indicates gzip compressed file) is very easy, doesn't
		// mean additional overhead and is actually safer to be on at all times.
		// If there are no gzipped files in the image, the code bellow will not cause any harm.

		// Check if requested file was GZIP compressed
		isGzip = espFsFlags(file) & FLAG_GZIP;
		if (isGzip) {
			// Check the browser's "Accept-Encoding" header. If the client does not
			// advertise that he accepts GZIP send a warning message (telnet users for e.g.)
			bool found = httpdGetHeader(connData, "Accept-Encoding", acceptEncodingBuffer, sizeof(acceptEncodingBuffer));
			if (!found || (strstr(acceptEncodingBuffer, "gzip") == NULL)) {
				//No Accept-Encoding: gzip header present
				httpdSend(connData, gzipNonSupportedMessage, -1);
				espFsClose(file);
				return HTTPD_CGI_DONE;
			}
		}

		connData->cgiData=file;
		httpdStartResponse(connData, 200);
		httpdHeader(connData, "Content-Type", httpdGetMimetype(filepath));
		if (isGzip) {
			httpdHeader(connData, "Content-Encoding", "gzip");
		}
		httpdHeader(connData, "Cache-Control", "max-age=3600, must-revalidate");
		httpdEndHeaders(connData);
		return HTTPD_CGI_MORE;
	}

	len=espFsRead(file, buff, FILE_CHUNK_LEN);
	if (len>0) httpdSend(connData, buff, len);
	if (len!=FILE_CHUNK_LEN) {
		//We're done.
		espFsClose(file);
		return HTTPD_CGI_DONE;
	} else {
		//Ok, till next time.
		return HTTPD_CGI_MORE;
	}
}


//This is a catch-all cgi function. It takes the url passed to it, looks up the corresponding
//path in the filesystem and if it exists, passes the file through. This simulates what a normal
//webserver would do with static files.
CgiStatus ICACHE_FLASH_ATTR cgiEspFsHook(HttpdConnData *connData) {
	const char *filepath = (connData->cgiArg == NULL) ? connData->url : (char *)connData->cgiArg;
	return serveStaticFile(connData, filepath);
}


//cgiEspFsTemplate can be used as a template.

typedef enum {
	ENCODE_PLAIN = 0,
	ENCODE_HTML,
	ENCODE_JS,
} TplEncode;

typedef struct {
	EspFsFile *file;
	void *tplArg;
	char token[64];
	int tokenPos;

	char buff[FILE_CHUNK_LEN + 1];

	bool chunk_resume;
	int buff_len;
	int buff_x;
	int buff_sp;
	char *buff_e;

	TplEncode tokEncode;
} TplData;

int ICACHE_FLASH_ATTR
tplSend(HttpdConnData *conn, const char *str, int len)
{
        if (conn == NULL) return 0;
        TplData *tpd=conn->cgiData;

        if (tpd == NULL || tpd->tokEncode == ENCODE_PLAIN) return httpdSend(conn, str, len);
        if (tpd->tokEncode == ENCODE_HTML) return httpdSend_html(conn, str, len);
        if (tpd->tokEncode == ENCODE_JS) return httpdSend_js(conn, str, len);
        return 0;
}

CgiStatus ICACHE_FLASH_ATTR cgiEspFsTemplate(HttpdConnData *connData) {
	TplData *tpd=connData->cgiData;
	int len;
	int x, sp=0;
	char *e=NULL;
	int tokOfs;

	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		((TplCallback)(connData->cgiArg))(connData, NULL, &tpd->tplArg);
		espFsClose(tpd->file);
		free(tpd);
		return HTTPD_CGI_DONE;
	}

	if (tpd==NULL) {
		//First call to this cgi. Open the file so we can read it.
		tpd=(TplData *)malloc(sizeof(TplData));
		if (tpd==NULL) {
			ESP_LOGE(TAG, "Failed to malloc tpl struct");
			return HTTPD_CGI_NOTFOUND;
		}

		tpd->chunk_resume = false;

		const char *filepath = connData->url;
		// check for custom template URL
		if (connData->cgiArg2 != NULL) {
			filepath = connData->cgiArg2;
			ESP_LOGD(TAG, "Using filepath %s", filepath);
		}

		tpd->file = espFsOpen(filepath);

		if (tpd->file == NULL) {
			// maybe a folder, look for index file
			tpd->file = tryOpenIndex(filepath);
			if (tpd->file == NULL) {
				free(tpd);
				return HTTPD_CGI_NOTFOUND;
			}
		}

		tpd->tplArg=NULL;
		tpd->tokenPos=-1;
		if (espFsFlags(tpd->file) & FLAG_GZIP) {
			ESP_LOGE(TAG, "cgiEspFsTemplate: Trying to use gzip-compressed file %s as template", connData->url);
			espFsClose(tpd->file);
			free(tpd);
			return HTTPD_CGI_NOTFOUND;
		}
		connData->cgiData=tpd;
		httpdStartResponse(connData, 200);
		const char *mime = httpdGetMimetype(connData->url);
		httpdHeader(connData, "Content-Type", mime);
		httpdAddCacheHeaders(connData, mime);
		httpdEndHeaders(connData);
		return HTTPD_CGI_MORE;
	}

	char *buff = tpd->buff;

	// resume the parser state from the last token,
	// if subst. func wants more data to be sent.
	if (tpd->chunk_resume) {
		//espfs_dbg("Resuming tpl parser for multi-part subst");
		len = tpd->buff_len;
		e = tpd->buff_e;
		sp = tpd->buff_sp;
		x = tpd->buff_x;
	} else {
		len = espFsRead(tpd->file, buff, FILE_CHUNK_LEN);
		tpd->buff_len = len;

		e = buff;
		sp = 0;
		x =  0;
	}

	if (len>0) {
		for (; x<len; x++) {
			if (tpd->tokenPos==-1) {
				//Inside ordinary text.
				if (buff[x]=='%') {
					//Send raw data up to now
					if (sp!=0) httpdSend(connData, e, sp);
					sp=0;
					//Go collect token chars.
					tpd->tokenPos=0;
				} else {
					sp++;
				}
			} else {
				if (buff[x]=='%') {
					if (tpd->tokenPos==0) {
						//This is the second % of a %% escape string.
						//Send a single % and resume with the normal program flow.
						httpdSend(connData, "%", 1);
					} else {
						if (!tpd->chunk_resume) {
							//This is an actual token.
							tpd->token[tpd->tokenPos++] = 0; //zero-terminate token

							tokOfs = 0;
							tpd->tokEncode = ENCODE_PLAIN;
							if (strncmp(tpd->token, "html:", 5) == 0) {
								tokOfs = 5;
								tpd->tokEncode = ENCODE_HTML;
							}
							else if (strncmp(tpd->token, "h:", 2) == 0) {
								tokOfs = 2;
								tpd->tokEncode = ENCODE_HTML;
							}
							else if (strncmp(tpd->token, "js:", 3) == 0) {
								tokOfs = 3;
								tpd->tokEncode = ENCODE_JS;
							}
							else if (strncmp(tpd->token, "j:", 2) == 0) {
								tokOfs = 2;
								tpd->tokEncode = ENCODE_JS;
							}

							// do the shifting
							if (tokOfs > 0) {
								for(int i=tokOfs; i<=tpd->tokenPos; i++) {
									tpd->token[i-tokOfs] = tpd->token[i];
								}
							}
						}

						tpd->chunk_resume = false;

						CgiStatus status = ((TplCallback)(connData->cgiArg))(connData, tpd->token, &tpd->tplArg);
						if (status == HTTPD_CGI_MORE) {
//							espfs_dbg("Multi-part tpl subst, saving parser state");
							// wants to send more in this token's place.....
							tpd->chunk_resume = true;
							tpd->buff_len = len;
							tpd->buff_e = e;
							tpd->buff_sp = sp;
							tpd->buff_x = x;
							break;
						}
					}
					//Go collect normal chars again.
					e=&buff[x+1];
					tpd->tokenPos=-1;
				}
				else {
					// Add char to the token buf
					char c = buff[x];
					bool outOfSpace = tpd->tokenPos >= (sizeof(tpd->token) - 1);
					if (outOfSpace ||
						(   !(c >= 'a' && c <= 'z') &&
							!(c >= 'A' && c <= 'Z') &&
							!(c >= '0' && c <= '9') &&
							c != '.' && c != '_' && c != '-' && c != ':'
						)) {
						// looks like we collected some garbage
						httpdSend(connData, "%", 1);
						if (tpd->tokenPos > 0) {
							httpdSend(connData, tpd->token, tpd->tokenPos);
						}
						// the bad char
						httpdSend(connData, &c, 1);

						//Go collect normal chars again.
						e=&buff[x+1];
						tpd->tokenPos=-1;
					}
					else {
						// collect it
						tpd->token[tpd->tokenPos++] = c;
					}
				}
			}
		}
	}

	if (tpd->chunk_resume) {
		return HTTPD_CGI_MORE;
	}

	//Send remaining bit.
	if (sp!=0) httpdSend(connData, e, sp);
	if (len!=FILE_CHUNK_LEN) {
		//We're done.
		((TplCallback)(connData->cgiArg))(connData, NULL, &tpd->tplArg);
		ESP_LOGD(TAG, "Template sent");
		espFsClose(tpd->file);
		free(tpd);
		return HTTPD_CGI_DONE;
	} else {
		//Ok, till next time.
		return HTTPD_CGI_MORE;
	}
}
#endif // CONFIG_ESPHTTPD_USE_ESPFS
