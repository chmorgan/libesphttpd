#pragma once

#include "cgi.h"


// macros for defining HttpdBuiltInUrl's

/** Route with a CGI handler and two arguments */
#define ROUTE_CGI_ARG2(path, handler, arg1, arg2)  {(path), (handler), (void *)(arg1), (void *)(arg2)}

/** Route with a CGI handler and one argument */
#define ROUTE_CGI_ARG(path, handler, arg1)         ROUTE_CGI_ARG2((path), (handler), (arg1), NULL)

/** Route with a CGI handler and an extended argument */
#define ROUTE_CGI_EX(path, handler, ex)            ROUTE_CGI_ARG2((path), (handler), &httpdCgiEx, (ex))

/** Route with an argument-less CGI handler */
#define ROUTE_CGI(path, handler)                   ROUTE_CGI_ARG2((path), (handler), NULL, NULL)

/** Static file route (file loaded from Vfs) */
#define ROUTE_FILE(path, filepath)                 ROUTE_CGI_ARG((path), cgiEspVfsGet, (const char*)(filepath))

/** Extended static file route (file loaded from Vfs) */
#define ROUTE_FILE_EX(path, ex)                    ROUTE_CGI_EX((path), cgiEspVfsGet, (HttpdCgiExArg*)(ex))

/** Static file as a template with a replacer function */
#define ROUTE_TPL(path, replacer)                  ROUTE_CGI_ARG2((path), cgiEspVfsTemplate, NULL, (TplCallback)(replacer))

/** Static file as a template with a replacer function, taking additional argument connData->cgiArg2 */
#define ROUTE_TPL_FILE(path, replacer, filepath)   ROUTE_CGI_ARG2((path), cgiEspVfsTemplate, (const char*)(filepath), (TplCallback)(replacer))

/** Redirect to some URL */
#define ROUTE_REDIRECT(path, target)               ROUTE_CGI_ARG((path), cgiRedirect, (const char*)(target))

/** Following routes are basic-auth protected */
#define ROUTE_AUTH(path, passwdFunc)               ROUTE_CGI_ARG((path), cgiAuthBasic, (AuthGetUserPw)(passwdFunc))

/** Websocket endpoint */
#define ROUTE_WS(path, callback)                   ROUTE_CGI_ARG((path), cgiWebsocket, (WsConnectedCb)(callback))

/** Catch-all filesystem route */
#define ROUTE_FILESYSTEM()                         ROUTE_CGI("*", cgiEspVfsGet)

#define ROUTE_END() {NULL, NULL, NULL, NULL}
