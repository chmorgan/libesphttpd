/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Connector to let httpd use the vfs filesystem to serve the files in it.
*/
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "libesphttpd/esp.h"
#include "libesphttpd/httpd.h"

#define FILE_CHUNK_LEN    1024

// If the client does not advertise that he accepts GZIP send following warning message (telnet users for e.g.)
static const char *gzipNonSupportedMessage = "HTTP/1.0 501 Not implemented\r\nServer: esp8266-httpd/"HTTPDVER"\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 52\r\n\r\nYour browser does not accept gzip-compressed data.\r\n";


//This is a catch-all cgi function. It takes the url passed to it, looks up the corresponding
//path in the filesystem and if it exists, passes the file through. This simulates what a normal
//webserver would do with static files.
CgiStatus ICACHE_FLASH_ATTR cgiEspVfsHook(HttpdConnData *connData) {
	FILE *file=connData->cgiData;
	int len;
	char buff[FILE_CHUNK_LEN];
	char filename[CONFIG_FATFS_MAX_LFN + 1];
	char acceptEncodingBuffer[64];
	int isGzip;
	bool isIndex = false;
	struct stat filestat;	

	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		fclose(file);
		return HTTPD_CGI_DONE;
	}

	//First call to this cgi.
	if (file==NULL) {
		if (connData->cgiArg != NULL) {
			strncpy(filename, connData->cgiArg, CONFIG_FATFS_MAX_LFN);
		} 
		strncat(filename, connData->url, CONFIG_FATFS_MAX_LFN - strlen(filename));
		ESP_LOGI(__func__, "GET: %s", filename);
		
		if(filename[strlen(filename)-1]=='/') filename[strlen(filename)-1]='\0';
		if(stat(filename, &filestat) == 0) {
			if((isIndex = S_ISDIR(filestat.st_mode))) {
				strncat(filename, "/index.html", CONFIG_FATFS_MAX_LFN - strlen(filename));
			}
		}
		
		file = fopen(filename, "r");
		isGzip = 0;
		
		if (file==NULL) {
			// Check if requested file is available GZIP compressed ie. with file extension .gz
		
			strncat(filename, ".gz", CONFIG_FATFS_MAX_LFN - strlen(filename));
			ESP_LOGI(__func__, "GET: GZIPped file - %s", filename);
			file = fopen(filename, "r");
			isGzip = 1;
			
			if (file==NULL) {
				return HTTPD_CGI_NOTFOUND;
			}				
		
			// Check the browser's "Accept-Encoding" header. If the client does not
			// advertise that he accepts GZIP send a warning message (telnet users for e.g.)
			httpdGetHeader(connData, "Accept-Encoding", acceptEncodingBuffer, 64);
			if (strstr(acceptEncodingBuffer, "gzip") == NULL) {
				//No Accept-Encoding: gzip header present
				httpdSend(connData, gzipNonSupportedMessage, -1);
				fclose(file);
				return HTTPD_CGI_DONE;
			}
		}

		connData->cgiData=file;
		httpdStartResponse(connData, 200);
		httpdHeader(connData, "Content-Type", isIndex?httpdGetMimetype("index.html"):httpdGetMimetype(connData->url));
		if (isGzip) {
			httpdHeader(connData, "Content-Encoding", "gzip");
		}
		httpdHeader(connData, "Cache-Control", "max-age=3600, must-revalidate");
		httpdEndHeaders(connData);
		return HTTPD_CGI_MORE;
	}

	len=fread(buff, 1, FILE_CHUNK_LEN, file);
	if (len>0) httpdSend(connData, buff, len);
	if (len!=FILE_CHUNK_LEN) {
		//We're done.
		fclose(file);
		return HTTPD_CGI_DONE;
	} else {
		//Ok, till next time.
		return HTTPD_CGI_MORE;
	}
}

typedef struct {
	FILE *file;
	void *tplArg;
	char token[64];
	int tokenPos;
} TplData;

typedef void (* TplCallback)(HttpdConnData *connData, char *token, void **arg);

CgiStatus ICACHE_FLASH_ATTR cgiEspVfsTemplate(HttpdConnData *connData) {
	TplData *tpd=connData->cgiData;
	int len;
	int x, sp=0;
	char *e=NULL;
	char buff[FILE_CHUNK_LEN +1];

	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		((TplCallback)(connData->cgiArg))(connData, NULL, &tpd->tplArg);
		fclose(tpd->file);
		free(tpd);
		return HTTPD_CGI_DONE;
	}

	if (tpd==NULL) {
		//First call to this cgi. Open the file so we can read it.
		tpd=(TplData *)malloc(sizeof(TplData));
		if (tpd==NULL) return HTTPD_CGI_NOTFOUND;
		tpd->file=fopen(connData->url, "r");
		tpd->tplArg=NULL;
		tpd->tokenPos=-1;
		if (tpd->file==NULL) {
			fclose(tpd->file);
			free(tpd);
			return HTTPD_CGI_NOTFOUND;
		}
		/*
		if (espFsFlags(tpd->file) & FLAG_GZIP) {
			httpd_printf("cgiEspFsTemplate: Trying to use gzip-compressed file %s as template!\n", connData->url);
			espFsClose(tpd->file);
			free(tpd);
			return HTTPD_CGI_NOTFOUND;
		}
		*/
		connData->cgiData=tpd;
		httpdStartResponse(connData, 200);
		httpdHeader(connData, "Content-Type", httpdGetMimetype(connData->url));
		httpdEndHeaders(connData);
		return HTTPD_CGI_MORE;
	}

	len=fread(buff, 1, FILE_CHUNK_LEN, tpd->file);
	if (len>0) {
		sp=0;
		e=buff;
		for (x=0; x<len; x++) {
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
						//This is an actual token.
						tpd->token[tpd->tokenPos++]=0; //zero-terminate token
						((TplCallback)(connData->cgiArg))(connData, tpd->token, &tpd->tplArg);
					}
					//Go collect normal chars again.
					e=&buff[x+1];
					tpd->tokenPos=-1;
				} else {
					if (tpd->tokenPos<(sizeof(tpd->token)-1)) tpd->token[tpd->tokenPos++]=buff[x];
				}
			}
		}
	}
	//Send remaining bit.
	if (sp!=0) httpdSend(connData, e, sp);
	if (len!=FILE_CHUNK_LEN) {
		//We're done.
		((TplCallback)(connData->cgiArg))(connData, NULL, &tpd->tplArg);
		fclose(tpd->file);
		free(tpd);
		return HTTPD_CGI_DONE;
	} else {
		//Ok, till next time.
		return HTTPD_CGI_MORE;
	}
}

CgiStatus   cgiEspVfsUpload(HttpdConnData *connData) {
    FILE *file=connData->cgiData;
    char filename[CONFIG_FATFS_MAX_LFN + 1];
    char output[FILE_CHUNK_LEN];	//Temporary buffer for HTML output
    int len;
    
    if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		fclose(file);
		return HTTPD_CGI_DONE;
	}

    //First call to this cgi.
	if (file==NULL) {
        
        if (connData->requestType!=HTTPD_METHOD_PUT) {
            httpdStartResponse(connData, 405);  //http error code 'Method Not Allowed'
            httpdEndHeaders(connData);
            return HTTPD_CGI_DONE;
        } else {
            strncpy(filename, connData->cgiArg, CONFIG_FATFS_MAX_LFN);
            strncat(filename, "/", CONFIG_FATFS_MAX_LFN - strlen(filename));
            strncat(filename, connData->getArgs, CONFIG_FATFS_MAX_LFN - strlen(filename));
            ESP_LOGI(__func__, "Uploading: %s", filename);

            file = fopen(filename, "w");
            connData->cgiData = file;
        }
    }

    ESP_LOGI(__func__, "Chunk: %d bytes, ", connData->post.buffLen);
    fwrite(connData->post.buff, 1, connData->post.buffLen, file);
    //todo: error check that bytes written == bufLen etc...
    if (connData->post.received == connData->post.len) {
		//We're done.
		fclose(file);
        ESP_LOGI(__func__, "Total: %d bytes written, ", connData->post.received);
        httpdStartResponse(connData, 200); 
        httpdHeader(connData, "Content-Type", "application/json");
        httpdEndHeaders(connData);

        httpdSend(connData, "{", -1);
        len = snprintf(output, FILE_CHUNK_LEN, "\"filename\": \"%s/%s\", ", (char *) connData->cgiArg, (char *) connData->getArgs);
        httpdSend(connData, output, len);
        len = snprintf(output, FILE_CHUNK_LEN, "\"size\": \"%d\" ", connData->post.received);
        httpdSend(connData, output, len);
        httpdSend(connData, "}", -1);
        return HTTPD_CGI_DONE;
	} else {
		//Ok, till next time.
		return HTTPD_CGI_MORE;
	}
}