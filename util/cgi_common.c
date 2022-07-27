//#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
#include <libesphttpd/esp.h>
#include "libesphttpd/httpd.h"
#include "libesphttpd/cgi_common.h"

#define SENDBUFSIZE (1024)
#define MAGICNUM	(0x12345678) // A magic number to validate the state object.  The value doesn't really matter, as long as it's unlikely to occur elsewhere.

typedef struct
{
	char *tofree;
	char *json_string;
	int len_to_send;
	uint32_t magic;
} cgiJsonResp_state_t;

// Common routine for parsing GET or POST parameters.  Parses *allArgs (i.e. connData->getArgs or connData->post.buff) 
// for a value by name of *argName and then converts the found string value to the requested binary format (sscanf format string).  
// (Must supply a buffer and then string value will be available on return.)
static bool cgiGetArgCommon(const char *allArgs, const char *argName, const char *format, void *pvalue, bool signd, char *buff, int buffLen)
{
	bool found = false;
	buff[0] = 0; // null terminate empty string

	int len = httpdFindArg(allArgs, argName, buff, buffLen);
	if (len > 0)
	{
		int n = 0;
		char ch; // dummy to test for malformed input
		if (signd)
		{
			int32_t val;
			n = sscanf(buff, format, &val, &ch);
			if (n == 1)
			{
				/* sscanf found a number to convert */
				*((int32_t *)pvalue) = val;
				found = true;
			}
		}
		else
		{
			uint32_t val;
			n = sscanf(buff, format, &val, &ch);
			if (n == 1)
			{
				/* sscanf found a number to convert */
				*((uint32_t *)pvalue) = val;
				found = true;
			}
		}
	}
	return found;
}

bool cgiGetArgDecS32(const char *allArgs, const char *argName, int *pvalue, char *buff, int buffLen)
{
	return cgiGetArgCommon(allArgs, argName, "%d%c", (void *)pvalue, true, buff, buffLen);
}
bool cgiGetArgDecU32(const char *allArgs, const char *argName, uint32_t *pvalue, char *buff, int buffLen)
{
	return cgiGetArgCommon(allArgs, argName, "%u%c", (void *)pvalue, false, buff, buffLen);
}
bool cgiGetArgHexU32(const char *allArgs, const char *argName, uint32_t *pvalue, char *buff, int buffLen)
{
	return cgiGetArgCommon(allArgs, argName, "%xl%c", (void *)pvalue, false, buff, buffLen);
}

bool cgiGetArgString(const char *allArgs, const char *argName, char *buff, int buffLen)
{
	bool found = false;

	int len = httpdFindArg(allArgs, argName, buff, buffLen);
	if (len > 0)
	{
		found = true;
	}
	return found;
}

void cgiJsonResponseHeaders(HttpdConnData *connData)
{
	//// Generate the header
	//We want the header to start with HTTP code 200, which means the document is found.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Cache-Control", "no-store, must-revalidate, no-cache, max-age=0");
	httpdHeader(connData, "Expires", "Mon, 01 Jan 1990 00:00:00 GMT");		  //  This one might be redundant, since modern browsers look for "Cache-Control".
	httpdHeader(connData, "Content-Type", "application/json; charset=utf-8"); //We are going to send some JSON.
	httpdEndHeaders(connData);
}

void cgiJavascriptResponseHeaders(HttpdConnData *connData)
{
	//// Generate the header
	//We want the header to start with HTTP code 200, which means the document is found.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Cache-Control", "no-store, must-revalidate, no-cache, max-age=0");
	httpdHeader(connData, "Expires", "Mon, 01 Jan 1990 00:00:00 GMT");				//  This one might be redundant, since modern browsers look for "Cache-Control".
	httpdHeader(connData, "Content-Type", "application/javascript; charset=utf-8"); //We are going to send a file as javascript.
	httpdEndHeaders(connData);
}

CgiStatus cgiJsonResponseCommonMulti(HttpdConnData *connData, void **statepp, cJSON *jsroot)
{
	cgiJsonResp_state_t *statep = NULL; // statep is local pointer to state
	if (statepp != NULL) // If statepp is passed in (from previous call), set local pointer.
	{
		statep = *(cgiJsonResp_state_t **)statepp; // dereference statepp to get statep
	}

	if (statep == NULL || statep->tofree == NULL)  // first call?
	{
		if (statep == NULL)
		{
			// statep is NULL, need to alloc memory for the state
			statep = calloc(1, sizeof(cgiJsonResp_state_t)); // all members init to 0
			if (statep == NULL)
			{
				ESP_LOGE(__func__, "calloc failed!");
				cJSON_Delete(jsroot); // Free memory since we can't use it
				return HTTPD_CGI_DONE;

			}
			statep->magic = MAGICNUM; // write a magic number to the state to validate it later
			if (statepp != NULL) // caller passed in pointer to statep?
			{
				*statepp = statep; // set external pointer for later
			}
		}

		if (jsroot)
		{
			statep->tofree = statep->json_string = cJSON_Print(jsroot);
			cJSON_Delete(jsroot); // we're done with the json object, now that it is stringified
		}

		if (statep->json_string)
		{
			statep->len_to_send = strlen(statep->json_string);
		}
		ESP_LOGD(__func__, "tosendtotal: %d", statep->len_to_send);
	}

	if (statep->magic != MAGICNUM) // check state magic number.  
	{
		// The state object which was referenced by statepp is invalid or corrupted!
		// The most likely reason for the magic mismatch is a programming mistake in the calling function.  
		//  Ensure statepp is preserved between calls to this function.
		ESP_LOGE(__func__, "state invalid!");
		// Probably not safe to free memory, so just shout error and return.
		return HTTPD_CGI_DONE;
	}

	if (statep->len_to_send > 0)
	{
		size_t max_send_size = SENDBUFSIZE;
		size_t len_to_send_this_time = (statep->len_to_send < max_send_size) ? (statep->len_to_send) : (max_send_size);
		ESP_LOGD(__func__, "tosendthistime: %d", len_to_send_this_time);
		int success = httpdSend(connData, statep->json_string, len_to_send_this_time);
		if (success)
		{
			statep->len_to_send -= len_to_send_this_time;
			statep->json_string += len_to_send_this_time;
		}
		else {
			ESP_LOGE(__func__, "httpdSend out-of-memory");
			statep->len_to_send = 0;
		}
	}

	if (statep->len_to_send <= 0 || // finished sending
		(statepp == NULL))		   // or called without state pointer (single send)
	{
		ESP_LOGD(__func__, "freeing");
		if (statep->tofree)
		{
			cJSON_free(statep->tofree);
			statep->tofree = NULL;
		}
		free(statep); // clear state
		statep = NULL;
		if (statepp != NULL) // was pointer to state passed in?
		{
			*statepp = NULL; // clear external pointer
		}

		return HTTPD_CGI_DONE;
	}
	return HTTPD_CGI_MORE;
}

CgiStatus cgiJsonResponseCommonSingle(HttpdConnData *connData, cJSON *jsroot)
{
	cgiJsonResponseHeaders(connData);
	return cgiJsonResponseCommonMulti(connData, NULL, jsroot);
}

CgiStatus cgiJavascriptResponseCommon(HttpdConnData *connData, cJSON *jsroot, const char *jsObjName)
{
	char sendbuff[SENDBUFSIZE];
	int sblen = 0;
	sendbuff[0] = 0; // null terminate empty string

	cgiJavascriptResponseHeaders(connData);
	sblen += snprintf(sendbuff + sblen, SENDBUFSIZE - sblen, "var %s = ", jsObjName);
	httpdSend(connData, sendbuff, sblen);

	return cgiJsonResponseCommonMulti(connData, NULL, jsroot);
}
