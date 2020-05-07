/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Connector to let httpd use the vfs filesystem to serve the files in it.
*/
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include "esp_err.h"
#include "esp_log.h"
#include "libesphttpd/esp.h"
#include "libesphttpd/httpd.h"
#include "httpd-platform.h"
#include "cJSON.h"

#define FILE_CHUNK_LEN    (1024)
#define MAX_FILENAME_LENGTH (1024)

#define ESPFS_MAGIC (0x73665345)
#define ESPFS_FLAG_GZIP (1<<1)

// If the client does not advertise that he accepts GZIP send following warning message (telnet users for e.g.)
static const char *gzipNonSupportedMessage = "HTTP/1.0 501 Not implemented\r\nServer: esp8266-httpd/"HTTPDVER"\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 52\r\n\r\nYour browser does not accept gzip-compressed data.\r\n";

static void cgiJsonResponseCommon(HttpdConnData *connData, cJSON *jsroot){
	char *json_string = NULL;

	//// Generate the header
	//We want the header to start with HTTP code 200, which means the document is found.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Cache-Control", "no-store, must-revalidate, no-cache, max-age=0");
	httpdHeader(connData, "Expires", "Mon, 01 Jan 1990 00:00:00 GMT");  //  This one might be redundant, since modern browsers look for "Cache-Control".
	httpdHeader(connData, "Content-Type", "application/json; charset=utf-8"); //We are going to send some JSON.
	httpdEndHeaders(connData);
	json_string = cJSON_Print(jsroot);
    if (json_string)
    {
    	httpdSend(connData, json_string, -1);
        cJSON_free(json_string);
    }
    cJSON_Delete(jsroot);
}

static size_t getFilepath(HttpdConnData *connData, char *filepath, size_t len)
{
	struct stat s;
	int outlen;

	if (connData->cgiArg != &httpdCgiEx) {
		filepath[0] = '\0';
		if (connData->cgiArg != NULL) {
			outlen = strlcpy(filepath, connData->cgiArg, len);
			if (stat(filepath, &s) == 0 && S_ISREG(s.st_mode)) {
				return outlen;
			}
		}
		return strlcat(filepath, connData->url, len);
	}

	HttpdCgiExArg *ex = (HttpdCgiExArg *)connData->cgiArg2;
	const char *route = connData->route;
	char *url = connData->url;
	while (*url && *route == *url) {
		route++;
		url++;
	}

	size_t basepathLen = strlen(ex->basepath);
	if (!ex->basepath || basepathLen == 0) {
		return strlcpy(filepath, url, len);
	}

	if (url[0] == '/') {
		url++;
	}

	outlen = strlcpy(filepath, ex->basepath, len);
	if (stat(ex->basepath, &s) != 0 || S_ISDIR(s.st_mode)) {
		if (ex->basepath[basepathLen - 1] != '/') {
			strlcat(filepath, "/", len);
		}
		outlen = strlcat(filepath, url, len);
	}
	return outlen;
}

CgiStatus ICACHE_FLASH_ATTR cgiEspVfsGet(HttpdConnData *connData) {
	FILE *file=connData->cgiData;
	int len;
	char buff[FILE_CHUNK_LEN];
	char filename[MAX_FILENAME_LENGTH + 1];
	char acceptEncodingBuffer[64];
	int isGzip;
	bool isIndex = false;
	struct stat filestat;	

	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		if(file != NULL){
			fclose(file);
			ESP_LOGD(__func__, "fclose: %s, r", filename);
		}
		ESP_LOGE(__func__, "Connection aborted!");
		return HTTPD_CGI_DONE;
	}

	//First call to this cgi.
	if (file==NULL) {
		isGzip = 0;
		if (connData->requestType!=HTTPD_METHOD_GET) {
			return HTTPD_CGI_NOTFOUND;  //	return and allow another cgi function to handle it
		}

		getFilepath(connData, filename, sizeof(filename));
		
		if(filename[strlen(filename)-1]=='/') filename[strlen(filename)-1]='\0';
		if(stat(filename, &filestat) == 0) {
			if((isIndex = S_ISDIR(filestat.st_mode))) {
				strncat(filename, "/index.html", MAX_FILENAME_LENGTH - strlen(filename));
			}
		}

		file = fopen(filename, "r");
		if (file != NULL) {
			struct stat st = {};
			fstat(fileno(file), &st);
			isGzip = (st.st_spare4[0] == ESPFS_MAGIC && st.st_spare4[1] & ESPFS_FLAG_GZIP);
			ESP_LOGD(__func__, "fopen: %s, r", filename);
		}
		
		if (file==NULL) {
			// Check if requested file is available GZIP compressed ie. with file extension .gz
		
			strncat(filename, ".gz", MAX_FILENAME_LENGTH - strlen(filename));
			ESP_LOGD(__func__, "GET: GZIPped file - %s", filename);
			file = fopen(filename, "r");
			if (file != NULL) ESP_LOGD(__func__, "fopen: %s, r", filename);
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
				ESP_LOGD(__func__, "fclose: %s, r", filename);
				ESP_LOGE(__func__, "client does not accept gzip!");
				return HTTPD_CGI_DONE;
			}
		}

		connData->cgiData=file;
		httpdStartResponse(connData, 200);

		const char *mimetype = NULL;
		bool sendContentType = false;
		bool sentHeaders = false;

		if (connData->cgiArg == &httpdCgiEx) {
			HttpdCgiExArg *ex = (HttpdCgiExArg *)connData->cgiArg2;
			if (ex->mimetype) {
				mimetype = ex->mimetype;
				sendContentType = true;
			} else if (!ex->headerCb) {
				sendContentType = true;
			}
		} else {
			sendContentType = true;
		}

		if (sendContentType) {
			if (!mimetype) {
				mimetype = isIndex ? httpdGetMimetype("index.html") : httpdGetMimetype(connData->url);
			}
			httpdHeader(connData, "Content-Type", mimetype);
		}

		if (isGzip) {
			httpdHeader(connData, "Content-Encoding", "gzip");
		}

		if (connData->cgiArg == &httpdCgiEx) {
			HttpdCgiExArg *ex = (HttpdCgiExArg *)connData->cgiArg2;
			if (ex->headerCb) {
				ex->headerCb(connData);
				sentHeaders = true;
			}
		}

		if (mimetype && !sentHeaders) {
			httpdAddCacheHeaders(connData, mimetype);
		}
		httpdEndHeaders(connData);
		return HTTPD_CGI_MORE;
	}

	len=fread(buff, 1, FILE_CHUNK_LEN, file);
	if (len>0) httpdSend(connData, buff, len);
	if (len!=FILE_CHUNK_LEN) {
		//We're done.
		fclose(file);
		ESP_LOGD(__func__, "fclose: %s, r", filename);

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
	char filename[MAX_FILENAME_LENGTH + 1];


	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		((TplCallback)(connData->cgiArg))(connData, NULL, &tpd->tplArg);
		if(tpd->file != NULL){
			fclose(tpd->file);
		}
		free(tpd);
		return HTTPD_CGI_DONE;
	}

	if (tpd==NULL) {
		//First call to this cgi. Open the file so we can read it.
		struct stat s;
		if (stat(filename, &s) != 0 || S_ISDIR(s.st_mode)) {
			return HTTPD_CGI_NOTFOUND;
		}

		if (s.st_spare4[0] == ESPFS_MAGIC && s.st_spare4[1] & ESPFS_FLAG_GZIP) {
			ESP_LOGE(__func__, "Trying to use gzip-compressed file %s as template!", connData->url);
			return HTTPD_CGI_NOTFOUND;
		}

		getFilepath(connData, filename, sizeof(filename));

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

		connData->cgiData=tpd;
		httpdStartResponse(connData, 200);

		const char *mimetype = NULL;
		bool sendContentType = false;

		if (connData->cgiArg == &httpdCgiEx) {
			HttpdCgiExArg *ex = (HttpdCgiExArg *)connData->cgiArg2;
			if (ex->mimetype) {
				mimetype = ex->mimetype;
				sendContentType = true;
			} else if (!ex->headerCb) {
				sendContentType = true;
			}
		} else {
			sendContentType = true;
		}

		if (sendContentType) {
			if (!mimetype) {
				mimetype = httpdGetMimetype(connData->url);
			}
			httpdHeader(connData, "Content-Type", mimetype);
		}

		if (connData->cgiArg == &httpdCgiEx) {
			HttpdCgiExArg *ex = (HttpdCgiExArg *)connData->cgiArg2;
			if (ex->headerCb) {
				ex->headerCb(connData);
			}
		}
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

static esp_err_t createMissingDirectories(char *fullpath) {
	char *string, *tofree;
	int err = ESP_OK;
	tofree = string = strndup(fullpath, MAX_FILENAME_LENGTH); // make a copy because modifies input
	assert(string != NULL);

	int i;
	for(i=0; string[i] != '\0'; i++) // search over all chars in fullpath
	{
		if (i>0 && (string[i] == '\\' || string[i] == '/')) // stop when reached a slash
		{
			struct stat sb;
			char slash = string[i];
			string[i] = '\0';  // replace slash with null terminator temporarily
			if (stat(string, &sb) != 0) { // stat() will tell us if it is a file or directory or neither.
				//printf("stat failed.\n");
				if (errno == ENOENT)  /* No such file or directory */
				{
					// Create directory
					int e = mkdir(string, S_IRWXU);
					if (e != 0)
					{
						ESP_LOGE(__func__, "mkdir failed; errno=%d\n",errno);
						err = ESP_FAIL;
						break;
					}
					else
					{
						ESP_LOGI(__func__, "created the directory %s\n",string);
					}
				}
		   }
		   string[i] = slash; // replace the slash and keep searching fullpath
	   }
	}
	free(tofree);  // don't skip this or memory-leak!
	return err;
}

typedef struct {
	enum {UPSTATE_START, UPSTATE_WRITE, UPSTATE_DONE, UPSTATE_ERR} state;
	FILE *file;
	char filename[MAX_FILENAME_LENGTH + 1];
	int b_written;
	const char *errtxt;
} UploadState;

CgiStatus   cgiEspVfsUpload(HttpdConnData *connData) {
	UploadState *state=(UploadState *)connData->cgiData;
    
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		if (state != NULL)
		{
			if(state->file != NULL){
				fclose(state->file);
				ESP_LOGD(__func__, "fclose: %s, r", state->filename);
			}
			free(state);
		}
		ESP_LOGE(__func__, "Connection aborted!");
		return HTTPD_CGI_DONE;
	}

	//First call to this cgi.
	if (state == NULL) {
		if (!(connData->requestType==HTTPD_METHOD_PUT || connData->requestType==HTTPD_METHOD_POST)) {
			return HTTPD_CGI_NOTFOUND;  //	return and allow another cgi function to handle it
		}
		//First call. Allocate and initialize state variable.
		state = malloc(sizeof(UploadState));
		if (state==NULL) {
			ESP_LOGE(__func__, "Can't allocate upload struct");
			//return HTTPD_CGI_NOTFOUND;  // Let cgiNotFound() deal with extra post data.
			state->state=UPSTATE_ERR;
			goto error_first;
		}
		memset(state, 0, sizeof(UploadState));  // all members of state are initialized to 0
		state->state = UPSTATE_START;

		if (connData->post.multipartBoundary != NULL)
		{
			// todo: handle when uploaded file is POSTed in a multipart/form-data.  For now, just upload the file by itself (i.e. xhr.send(file), not xhr.send(form)).
			ESP_LOGE(__func__, "Sorry! file upload in multipart/form-data is not supported yet.");
			//return HTTPD_CGI_NOTFOUND;  // Let cgiNotFound() deal with extra post data.
			state->state=UPSTATE_ERR;
			goto error_first;
		}

		// cgiArg specifies where this function is allowed to write to.  It must be specified and can't be empty.
		const char *basePath = connData->cgiArg;
		if ((basePath == NULL) || (*basePath == 0)) {
			state->state=UPSTATE_ERR;
			goto error_first;
		}
		int n = strlcpy(state->filename, basePath, MAX_FILENAME_LENGTH);
		if (n >= MAX_FILENAME_LENGTH) goto error_first;

		// Is cgiArg a single file or a directory (with trailing slash)?
		if (basePath[n - 1] == '\\' || basePath[n - 1] == '/') // check last char in cgiArg string for a slash
		{
			// Last char of cgiArg is a slash, assume it is a directory.

			// get queryParameter "filename" : string
			char* filenamebuf = state->filename + n;
		    int arglen = httpdFindArg(connData->getArgs, "filename", filenamebuf, MAX_FILENAME_LENGTH - n);
		    // 3. (highest priority) Filename to write to is cgiArg + "filename" as specified by url parameter
		    if (arglen > 0)
		    {
		    	// filename is already appended by httpdFindArg() above.
		    }
		    // 2. Filename to write to is cgiArg + "filename" as inside multipart/form-data --todo)
		    else if (0)
		    {
				// Beginning of POST data (after first headers):
				/*
				-----------------------------190192493010810\r\n
				Content-Disposition: form-data; name="file"; filename="/README.md"\r\n
				Content-Type: application/octet-stream\r\n
				\r\n
				datadatadatadata...
	            */
		    }
		    // 1: Filename to write to is cgiArg + url.
		    else if(connData->url != NULL){
				strncat(state->filename, connData->url, MAX_FILENAME_LENGTH - n);
			}
		}
		else
		{
			// Last char of cgiArg is not a slash, assume a single file.
			// Filename to write to is forced to cgiArg.  The filename specified in the PUT url or in the POST filename is simply ignored.
			//   (Anyway, a proper ROUTE entry should enforce the PUT url matches the cgiArg. i.e.
			//   ROUTE_CGI_ARG("/writeable_file.txt", cgiEspVfsPut, FS_BASE_PATH "/html/writeable_file.txt"))
			// filename is already = basePath from strlcpy() above.
		}


		ESP_LOGI(__func__, "Uploading: %s", state->filename);

		// Create missing directories
		if (createMissingDirectories(state->filename) != ESP_OK)
		{
			state->errtxt="Error creating directory!";
			state->state=UPSTATE_ERR;
			goto error_first;
		}

		// Open file for writing
		state->file = fopen(state->filename, "w");
		if (state->file == NULL)
		{
			ESP_LOGE(__func__, "Can't open file for writing!");
			state->errtxt="Can't open file for writing!";
			state->state=UPSTATE_ERR;
			goto error_first;
		}

		state->state=UPSTATE_WRITE;
		ESP_LOGD(__func__, "fopen: %s, w", state->filename);

error_first:
		connData->cgiData=state;
	}

	ESP_LOGD(__func__, "Chunk: %d bytes, ", connData->post.buffLen);

	if (state->state==UPSTATE_WRITE) {
		if(state->file != NULL){
			int count = fwrite(connData->post.buff, 1, connData->post.buffLen, state->file);
			state->b_written += count;
			if (count != connData->post.buffLen)
			{
				state->state=UPSTATE_ERR;
				ESP_LOGE(__func__, "error writing to filesystem!");
			}
			if (state->b_written >= connData->post.len){
				state->state=UPSTATE_DONE;
			}
		} // else, Just eat up any bytes we receive.
	} else if (state->state==UPSTATE_DONE) {
		ESP_LOGE(__func__, "bogus bytes received after data received");
		//Ignore those bytes.
	} else if (state->state==UPSTATE_ERR) {
		//Just eat up any bytes we receive.
	}

	if (connData->post.received == connData->post.len) {
		//We're done.
		cJSON *jsroot = cJSON_CreateObject();
		if(state->file != NULL){
			fclose(state->file);
			ESP_LOGD(__func__, "fclose: %s, r", state->filename);
		}
		ESP_LOGI(__func__, "Total: %d bytes written.", state->b_written);

		cJSON_AddStringToObject(jsroot, "filename", state->filename);
		cJSON_AddNumberToObject(jsroot, "bytes received", connData->post.received);
		cJSON_AddNumberToObject(jsroot, "bytes written", state->b_written);
		cJSON_AddBoolToObject(jsroot, "success", state->state==UPSTATE_DONE);
		free(state);

		cgiJsonResponseCommon(connData, jsroot); // Send the json response!
		return HTTPD_CGI_DONE;
	} else {
		//Ok, till next time.
		return HTTPD_CGI_MORE;
	}
}
