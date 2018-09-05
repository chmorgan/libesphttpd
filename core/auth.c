/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
HTTP auth implementation. Only does basic authentication for now.
*/

#ifdef linux
#include <libesphttpd/linux.h>
#else
#include <libesphttpd/esp.h>
#endif

#include "libesphttpd/auth.h"
#include "libesphttpd_base64.h"

CgiStatus ICACHE_FLASH_ATTR authBasic(HttpdConnData *connData) {
	const char *unauthorized = "401 Unauthorized.";
	int no=0;
	int r;
	char hdr[(AUTH_MAX_USER_LEN+AUTH_MAX_PASS_LEN+2)*10];
	char userpass[AUTH_MAX_USER_LEN+AUTH_MAX_PASS_LEN+2];
	char user[AUTH_MAX_USER_LEN];
	char pass[AUTH_MAX_PASS_LEN];

	if(connData->isConnectionClosed)
	{
		//Connection closed. Clean up.
		return HTTPD_CGI_DONE;
	}

	r=httpdGetHeader(connData, "Authorization", hdr, sizeof(hdr));
	if (r && strncmp(hdr, "Basic", 5)==0) {
		r=libesphttpd_base64_decode(strlen(hdr)-6, hdr+6, sizeof(userpass), (unsigned char *)userpass);
		if (r<0) r=0; //just clean out string on decode error
		userpass[r]=0; //zero-terminate user:pass string
//		printf("Auth: %s\n", userpass);
		while (((AuthGetUserPw)(connData->cgiArg))(connData, no,
				user, AUTH_MAX_USER_LEN, pass, AUTH_MAX_PASS_LEN)) {
			//Check user/pass against auth header
			if (strlen(userpass)==strlen(user)+strlen(pass)+1 &&
					strncmp(userpass, user, strlen(user))==0 &&
					userpass[strlen(user)]==':' &&
					strcmp(userpass+strlen(user)+1, pass)==0) {
				//Authenticated. Yay!
				return HTTPD_CGI_AUTHENTICATED;
			}
			no++; //Not authenticated with this user/pass. Check next user/pass combo.
		}
	}

	//Not authenticated. Go bug user with login screen.
	httpdStartResponse(connData, 401);
	httpdHeader(connData, "Content-Type", "text/plain");
	httpdHeader(connData, "WWW-Authenticate", "Basic realm=\""HTTP_AUTH_REALM"\"");
	httpdEndHeaders(connData);
	httpdSend(connData, unauthorized, -1);
	//Okay, all done.
	return HTTPD_CGI_DONE;
}
