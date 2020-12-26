#pragma once

#include "httpd.h"

//This is a catch-all cgi function. It takes the url passed to it, looks up the corresponding
//path in the filesystem and if it exists, passes the file through. This simulates what a normal
//webserver would do with static files.
// If the file is not found, (or if http method is not GET) this cgi function returns NOT_FOUND, and then other cgi functions specified later in the routing table can try.
// The cgiArg value is the base directory path, if specified.
//
// Usage:
//      ROUTE_CGI("*", cgiEspVfsGet) or
//      ROUTE_CGI_ARG("*", cgiEspVfsGet, "/base/directory/") or
//      ROUTE_CGI_ARG("*", cgiEspVfsGet, ".") to use the current working directory
CgiStatus cgiEspVfsGet(HttpdConnData *connData);


//This is a POST and PUT handler for uploading files to the VFS filesystem.
// If http method is not PUT or POST, this cgi function returns NOT_FOUND, and then other cgi functions specified later in the routing table can try.
// Specify base directory (with trailing slash) or single file as 1st cgiArg.
//
// Filename can be specified 3 ways, in order of priority lowest to highest:
//  1.  URL Path.  i.e. PUT http://1.2.3.4/path/newfile.txt
//  2.  Inside multipart/form-data (todo not supported yet)
//  3.  URL Parameter.  i.e. POST http://1.2.3.4/upload.cgi?filename=path%2Fnewfile.txt
//
// Usage:
//      ROUTE_CGI_ARG("*", cgiEspVfsUpload, "/base/directory/")
//           - Allows creating/replacing files anywhere under "/base/directory/".  Don't forget to specify trailing slash in cgiArg!
//           - example: POST or PUT http://1.2.3.4/anydir/anyname.txt
//
//      ROUTE_CGI_ARG("/filesystem/upload.cgi", cgiEspVfsUpload, "/base/directory/")
//           - Allows creating/replacing files anywhere under "/base/directory/".  Don't forget to specify trailing slash in cgiArg!
//           - example: POST or PUT http://1.2.3.4/filesystem/upload.cgi?filename=newdir%2Fnewfile.txt
//
//      ROUTE_CGI_ARG("/writeable_file.txt", cgiEspVfsUpload, "/base/directory/writeable_file.txt")
//           - Allows only replacing content of one file at "/base/directory/writeable_file.txt".
//           - example: POST or PUT http://1.2.3.4/writeable_file.txt
CgiStatus cgiEspVfsUpload(HttpdConnData *connData);
