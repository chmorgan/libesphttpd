#include <inttypes.h>
// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
#include <libesphttpd/esp.h>
#include "libesphttpd/httpd.h"
#include "libesphttpd/cgi_common.h"

#define SENDBUFSIZE (1024)
#define MAGICNUM (0x12345678) // A magic number to validate the state object.  The value doesn't really matter, as long as it's unlikely to occur elsewhere.

typedef struct
{
	void *toFree; // keep a pointer to the beginning for free() when finished
	const char *toSendPosition;
	int len_to_send;
	uint32_t magic;
} cgiResp_state_t;

/**
 * @brief  Common routine for parsing GET or POST parameters.
 * Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a value
 * by name of *argName and then converts the found string value to the requested binary format (sscanf format string).
 * (Must supply a buffer and then string value will be available on return.)
 *
 * @param allArgs String to search in i.e. connData->getArgs or connData->post.buff
 * @param argName Name of argument to find
 * @param format sscanf format string
 * @param pvalue return value parsed
 * @param signd Should the return value be treated as signed?
 * @param buff Supply a buffer to parse the value found.  The unparsed value will be available here upon return if found.
 * @param buffLen Length of supplied buffer
 * @return int 0: arg not found, 1: arg found and parsed, -1: found arg but error parsing value
 */
static int cgiGetArgCommon(const char *allArgs, const char *argName, const char *format, void *pvalue, bool signd, char *buff, int buffLen)
{
	int retVal = 0;
	buff[0] = 0; // null terminate empty string
	int len = httpdFindArg(allArgs, argName, buff, buffLen);
	if (len > 0)
	{
		int n = 0;
		char ch; // dummy to test for malformed input
		if (signd)
		{
			n = sscanf(buff, format, (int32_t *)pvalue, &ch);
		}
		else
		{
			n = sscanf(buff, format, (uint32_t *)pvalue, &ch);
		}
		if (n == 1)
		{
			/* sscanf found a number to convert */
			retVal = 1;
		}
		else
		{
			retVal = -1; // error parsing number
		}
	}
	return retVal;
}

/**
 * @brief Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a signed integer by name of *argName and returns int value at *pvalue.
 *
 * @param allArgs String to search in i.e. connData->getArgs or connData->post.buff
 * @param argName Name of argument to find
 * @param pvalue return value parsed
 * @param buff Supply a buffer to parse the value found.  The unparsed value will be available here upon return if found.
 * @param buffLen Length of supplied buffer
 * @return int 0: arg not found, 1: arg found and parsed, -1: found arg but error parsing value
 */
int cgiGetArgDecS32(const char *allArgs, const char *argName, int *pvalue, char *buff, int buffLen)
{
	return cgiGetArgCommon(allArgs, argName, "%" PRId32 "%c", (void *)pvalue, true, buff, buffLen);
}

/**
 * @brief Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a unsigned int (i.e. ?uintval=123)
 *
 * @param allArgs String to search in i.e. connData->getArgs or connData->post.buff
 * @param argName Name of argument to find
 * @param pvalue return value parsed
 * @param buff Supply a buffer to parse the value found.  The unparsed value will be available here upon return if found.
 * @param buffLen Length of supplied buffer
 * @return int 0: arg not found, 1: arg found and parsed, -1: found arg but error parsing value
 */
int cgiGetArgDecU32(const char *allArgs, const char *argName, uint32_t *pvalue, char *buff, int buffLen)
{
	return cgiGetArgCommon(allArgs, argName, "%" PRIu32 "%c", (void *)pvalue, false, buff, buffLen);
}

/**
 * @brief Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a uint32_t from a hexadecimal string (i.e. ?hexval=0123ABCD )
 *
 * @param allArgs String to search in i.e. connData->getArgs or connData->post.buff
 * @param argName Name of argument to find
 * @param pvalue return value parsed
 * @param buff Supply a buffer to parse the value found.  The unparsed value will be available here upon return if found.
 * @param buffLen Length of supplied buffer
 * @return int 0: arg not found, 1: arg found and parsed, -1: found arg but error parsing value
 */
int cgiGetArgHexU32(const char *allArgs, const char *argName, uint32_t *pvalue, char *buff, int buffLen)
{
	return cgiGetArgCommon(allArgs, argName, "%" PRIx32 "%c", (void *)pvalue, false, buff, buffLen);
}

/**
 * @brief Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a string value.  (just a wrapper for httpdFindArg())
 *
 * @param allArgs String to search in i.e. connData->getArgs or connData->post.buff
 * @param argName Name of argument to find
 * @param buff Supply a buffer to copy the value found.
 * @param buffLen Length of supplied buffer.
 * @return int 0: arg not found, 1: arg found
 */
int cgiGetArgString(const char *allArgs, const char *argName, char *buff, int buffLen)
{
	int retVal = 0;
	int len = httpdFindArg(allArgs, argName, buff, buffLen);
	if (len > 0)
	{
		retVal = 1;
	}
	return retVal;
}

void cgiJsonResponseHeaders(HttpdConnData *connData)
{
	//// Generate the header
	// We want the header to start with HTTP code 200, which means the document is found.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Cache-Control", "no-store, must-revalidate, no-cache, max-age=0");
	httpdHeader(connData, "Expires", "Mon, 01 Jan 1990 00:00:00 GMT");		  //  This one might be redundant, since modern browsers look for "Cache-Control".
	httpdHeader(connData, "Content-Type", "application/json; charset=utf-8"); // We are going to send some JSON.
	httpdEndHeaders(connData);
}

void cgiJavascriptResponseHeaders(HttpdConnData *connData)
{
	//// Generate the header
	// We want the header to start with HTTP code 200, which means the document is found.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Cache-Control", "no-store, must-revalidate, no-cache, max-age=0");
	httpdHeader(connData, "Expires", "Mon, 01 Jan 1990 00:00:00 GMT");				//  This one might be redundant, since modern browsers look for "Cache-Control".
	httpdHeader(connData, "Content-Type", "application/javascript; charset=utf-8"); // We are going to send a file as javascript.
	httpdEndHeaders(connData);
}

CgiStatus cgiResponseCommonMultiCleanup(void **statepp)
{
	cgiResp_state_t *statep = NULL; // statep is local pointer to state
	if (statepp != NULL)
	{
		statep = *(cgiResp_state_t **)statepp; // dereference statepp to get statep
		if (statep)
		{
			if (statep->toFree)
			{
				ESP_LOGD(__func__, "freeing");
				free(statep->toFree);
			}
			free(statep); // clear state
		}
		*statepp = NULL; // clear external pointer
	}
	return HTTPD_CGI_DONE;
}
/**
 * @brief Send a string that is longer than can fit in a single call to httpdSend().  The string is freed when done.
 *
 * @param connData HttpdConnData
 * @param statepp Opaque pointer-pointer state
 * @param toSendAndFree String buffer. Note this function will call free() on this string when finished!
 * @return CgiStatus HTTPD_CGI_DONE: no need to call again.  HTTPD_CGI_MORE: Need to call this again to finish sending.
 */
CgiStatus cgiResponseCommonMulti(HttpdConnData *connData, void **statepp, char *toSendAndFree)
{
	cgiResp_state_t *statep = NULL; // statep is local pointer to state
	if (statepp != NULL)			// If statepp is passed in (from previous call), set local pointer.
	{
		statep = *(cgiResp_state_t **)statepp; // dereference statepp to get statep
	}

	if (statep == NULL) // first call?
	{
		// statep is NULL, need to alloc memory for the state
		statep = calloc(1, sizeof(cgiResp_state_t)); // all members init to 0
		if (statep == NULL)
		{
			ESP_LOGE(__func__, "calloc failed!");
			return HTTPD_CGI_DONE;
		}
		statep->magic = MAGICNUM; // write a magic number to the state to validate it later
		if (statepp != NULL)	  // caller passed in pointer to statep?
		{
			*statepp = statep; // set external pointer for later
		}

		statep->toFree = toSendAndFree;
		statep->toSendPosition = toSendAndFree;

		if (statep->toSendPosition)
		{
			statep->len_to_send = strlen(statep->toSendPosition);
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
		int success = httpdSend(connData, statep->toSendPosition, len_to_send_this_time);
		if (success)
		{
			statep->len_to_send -= len_to_send_this_time;
			statep->toSendPosition += len_to_send_this_time;
		}
		else
		{
			ESP_LOGE(__func__, "httpdSend out-of-memory");
			statep->len_to_send = 0;
		}
	}

	// if called without state pointer (single send)
	if (NULL == statepp)
	{
		cgiResponseCommonMultiCleanup(statep);
		return HTTPD_CGI_DONE;
	}
	// else if finished sending
	if (statep->len_to_send <= 0)
	{
		cgiResponseCommonMultiCleanup(statepp);
		return HTTPD_CGI_DONE;
	}
	// else still sending
	return HTTPD_CGI_MORE;
}

/**
 * @brief Send a multipart json response (i.e. larger than 1kB). The json object is freed when done.
 *
 * @param connData HttpdConnData
 * @param statepp Opaque pointer-pointer state
 * @param jsroot cJSON object to send
 * @return CgiStatus HTTPD_CGI_DONE: no need to call again.  HTTPD_CGI_MORE: Need to call this again to finish sending.
 *
 * @example
		CgiStatus cgiFn(HttpdConnData *connData)
		{
			if (connData->isConnectionClosed)
			{
				// Connection aborted. Clean up.
				cgiResponseCommonMultiCleanup(&connData->cgiData);
				return HTTPD_CGI_DONE;
			}
			cJSON *jsroot = NULL;
			if (connData->cgiData == NULL)
			{
				//First call to this cgi.
				jsroot = cJSON_CreateObject();
				...
				cgiJsonResponseHeaders(connData);
			}
			return cgiJsonResponseCommonMulti(connData, &connData->cgiData, jsroot); // Send the json response!
		}
 */
CgiStatus cgiJsonResponseCommonMulti(HttpdConnData *connData, void **statepp, cJSON *jsroot)
{
	// This wrapper for cgiResponseCommonMulti() doesn't need it's own state.
	// We can determine if this is the first call by testing **statepp

	char *stringToSend = NULL;
	if (statepp == NULL || *statepp == NULL) // First call?
	{
		stringToSend = cJSON_PrintUnformatted(jsroot);
		cJSON_Delete(jsroot); // we're done with the json object, now that it is stringified
	}

	return cgiResponseCommonMulti(connData, statepp, stringToSend); // will free stringToSend when done
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
