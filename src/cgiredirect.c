#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#ifndef LINUX
# include <esp_netif.h>
#endif

#include "libesphttpd/cgi.h"


//Use this as a cgi function to redirect one url to another.
CgiStatus cgiRedirect(HttpdConnData *connData) {
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	httpdRedirect(connData, (char*)connData->cgiArg);
	return HTTPD_CGI_DONE;
}

CgiStatus cgiRedirectToHostname(HttpdConnData *connData) {
	static const char hostFmt[]="http://%s/";
	char *buff;
	int isIP=0;
	int x;
	if (connData->isConnectionClosed) {
		// Connection closed.
		return HTTPD_CGI_DONE;
	}
	if (connData->hostName==NULL) {
		return HTTPD_CGI_NOTFOUND;
	}

    //Quick and dirty code to see if host is an IP
    if (strlen(connData->hostName)>8)
    {
        isIP=1;
        for (x=0; x<strlen(connData->hostName); x++) {
            if (connData->hostName[x]!='.' && (connData->hostName[x]<'0' || connData->hostName[x]>'9')) isIP=0;
        }
    }

    if (isIP)
    {
        return HTTPD_CGI_NOTFOUND;
    }

    //Check hostname; pass on if the same
    if (strcasecmp(connData->hostName, (char*)connData->cgiArg)==0)
    {
        ESP_LOGD(__func__, "connData->hostName:'%s', redirect hostname: '%s'", connData->hostName,
                (char*)connData->cgiArg);
        return HTTPD_CGI_NOTFOUND;
    }

	//Not the same. Redirect to real hostname.
	buff = malloc(strlen((char*)connData->cgiArg)+sizeof(hostFmt));
	if (buff==NULL) {
        ESP_LOGE(__func__, "allocating memory");
		//Bail out
		return HTTPD_CGI_DONE;
	}
	sprintf(buff, hostFmt, (char*)connData->cgiArg);
	ESP_LOGD(__func__, "Redirecting to hostname url %s", buff);
	httpdRedirect(connData, buff);
	free(buff);
	return HTTPD_CGI_DONE;
}

//Same as above, but will only redirect clients with an IP that is in the range of
//the SoftAP interface. This should preclude clients connected to the STA interface
//to be redirected to nowhere.
CgiStatus cgiRedirectApClientToHostname(HttpdConnData *connData) {
	/* TODO: Implement this */
	return HTTPD_CGI_NOTFOUND;
}
