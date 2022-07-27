/* Functions commonly used in cgi handlers for libesphttpd */

#ifndef CGI_COMMON_H
#define CGI_COMMON_H

#include "libesphttpd/httpd.h"
#include "cJSON.h"

// Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a signed integer by name of *argName and returns int value at *pvalue.  
bool cgiGetArgDecS32(const char *allArgs, const char *argName, int *pvalue, char *buff, int buffLen);
// Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a unsigned int (i.e. ?uintval=123)
bool cgiGetArgDecU32(const char *allArgs, const char *argName, uint32_t *pvalue, char *buff, int buffLen);
// Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a uint32_t from a hexadecimal string (i.e. ?hexval=0123ABCD )
bool cgiGetArgHexU32(const char *allArgs, const char *argName, uint32_t *pvalue, char *buff, int buffLen);
// Parses *allArgs (i.e. connData->getArgs or connData->post.buff) for a string value.  (just a wrapper for httpdFindArg())
bool cgiGetArgString(const char *allArgs, const char *argName, char *buff, int buffLen);


void cgiJsonResponseHeaders(HttpdConnData *connData);
void cgiJavascriptResponseHeaders(HttpdConnData *connData);

/**
 * Example usage of cgiJsonResponseCommonMulti for multipart json response (i.e. larger than 1kB)
 * 
CgiStatus cgiFn(HttpdConnData *connData)
{
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
CgiStatus cgiJsonResponseCommonMulti(HttpdConnData *connData, void **statepp, cJSON *jsroot);
CgiStatus cgiJsonResponseCommonSingle(HttpdConnData *connData, cJSON *jsroot);

CgiStatus cgiJavascriptResponseCommon(HttpdConnData *connData, cJSON *jsroot, const char *jsObjName);

#endif //CGI_COMMON_H
