#include <libesphttpd/esp.h>
#include <libesphttpd/cgiredirect.h>

#include "esp_log.h"

const static char* TAG = "cgiredirect";


//Use this as a cgi function to redirect one url to another.
CgiStatus ICACHE_FLASH_ATTR cgiRedirect(HttpdConnData *connData) {
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	httpdRedirect(connData, (char*)connData->cgiArg);
	return HTTPD_CGI_DONE;
}

CgiStatus ICACHE_FLASH_ATTR cgiRedirectToHostname(HttpdConnData *connData) {
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
        ESP_LOGD(TAG, "connData->hostName:'%s', redirect hostname: '%s'", connData->hostName,
                (char*)connData->cgiArg);
        return HTTPD_CGI_NOTFOUND;
    }

	//Not the same. Redirect to real hostname.
	buff = malloc(strlen((char*)connData->cgiArg)+sizeof(hostFmt));
	if (buff==NULL) {
        ESP_LOGE(TAG, "allocating memory");
		//Bail out
		return HTTPD_CGI_DONE;
	}
	sprintf(buff, hostFmt, (char*)connData->cgiArg);
	ESP_LOGD(TAG, "Redirecting to hostname url %s", buff);
	httpdRedirect(connData, buff);
	free(buff);
	return HTTPD_CGI_DONE;
}


//Same as above, but will only redirect clients with an IP that is in the range of
//the SoftAP interface. This should preclude clients connected to the STA interface
//to be redirected to nowhere.
CgiStatus ICACHE_FLASH_ATTR cgiRedirectApClientToHostname(HttpdConnData *connData) {
#ifdef linux
	return HTTPD_CGI_NOTFOUND;
#else
#ifndef FREERTOS
	uint32 *remadr;
	struct ip_info apip;
	int x=wifi_get_opmode();
	//Check if we have an softap interface; bail out if not
	if (x!=2 && x!=3) return HTTPD_CGI_NOTFOUND;
	remadr=(uint32 *)connData->remote_ip;
	wifi_get_ip_info(SOFTAP_IF, &apip);
	if ((*remadr & apip.netmask.addr) == (apip.ip.addr & apip.netmask.addr)) {
		return cgiRedirectToHostname(connData);
	} else {
		return HTTPD_CGI_NOTFOUND;
	}
#else
	return HTTPD_CGI_NOTFOUND;
#endif
#endif
}
