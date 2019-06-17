#pragma once

#include "httpd.h"

#ifdef FREERTOS
#ifdef ESP32
#include "lwip/sockets.h"
#else
#include "lwip/lwip/sockets.h"
#endif
#endif // #ifdef FREERTOS

#ifdef CONFIG_ESPHTTPD_SSL_SUPPORT
#include <openssl/ssl.h>
#ifdef linux
#include <openssl/err.h>
#endif
#endif

#ifdef linux
#include <netinet/in.h>
#endif


#ifdef linux
    #define PLAT_RETURN void*
#else
    #define PLAT_RETURN void
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct RtosConnType{
	int fd;
	int needWriteDoneNotif;
	int needsClose;
	int port;
	char ip[4];
#ifdef CONFIG_ESPHTTPD_SSL_SUPPORT
	SSL *ssl;
#endif

	// server connection data structure
	HttpdConnData connData;
};

#define RECV_BUF_SIZE 2048

typedef struct
{
    RtosConnType *rconn;

    int httpPort;
    struct sockaddr_in httpListenAddress;
    HttpdFlags httpdFlags;

#ifdef CONFIG_ESPHTTPD_SHUTDOWN_SUPPORT
	int udpShutdownPort;
#endif

	bool isShutdown;

	// storage for data read in the main loop
	char precvbuf[RECV_BUF_SIZE];

#ifdef linux
    pthread_mutex_t httpdMux;
#else
    xQueueHandle httpdMux;
#endif

#ifdef CONFIG_ESPHTTPD_SSL_SUPPORT
    SSL_CTX *ctx;
#endif

    HttpdInstance httpdInstance;
} HttpdFreertosInstance;

typedef struct {
    bool shutdown;
    bool listeningForNewConnections;
    char serverStr[20];
    struct timeval *selectTimeoutData;
    HttpdFreertosInstance *pInstance;
    int32 listenFd;
    int32 udpListenFd;
    int32 remoteFd;
} ServerTaskContext;

/**
 * Execute the server task in a loop, internally calls init, process and deinit
 */
PLAT_RETURN platHttpServerTask(void *pvParameters);

/**
 * Manually init all data required for processing the server task
 */
void platHttpServerTaskInit(ServerTaskContext *ctx, HttpdFreertosInstance *pInstance);

/**
 * Manually execute the server task loop function once
 */
void platHttpServerTaskProcess(ServerTaskContext *ctx);

/**
 * Manually deinit all data required for processing the server task
 */
PLAT_RETURN platHttpServerTaskDeinit(ServerTaskContext *ctx);


/*
 * connectionBuffer should be sized 'sizeof(RtosConnType) * maxConnections'
 */
HttpdInitStatus httpdFreertosInit(HttpdFreertosInstance *pInstance,
                                const HttpdBuiltInUrl *fixedUrls,
                                int port,
                                void* connectionBuffer, int maxConnections,
                                HttpdFlags flags);

/* NOTE: listenAddress is in network byte order
 *
 * connectionBuffer should be sized 'sizeof(RtosConnType) * maxConnections'
 */
HttpdInitStatus httpdFreertosInitEx(HttpdFreertosInstance *pInstance,
                                    const HttpdBuiltInUrl *fixedUrls,
                                    int port,
                                    uint32_t listenAddress,
                                    void* connectionBuffer, int maxConnections,
                                    HttpdFlags flags);


typedef enum
{
	StartSuccess,
	StartFailedSslNotConfigured
} HttpdStartStatus;

/**
 * Call to start the server
 */
HttpdStartStatus httpdFreertosStart(HttpdFreertosInstance *pInstance);

typedef enum
{
    SslInitSuccess,
    SslInitContextCreationFailed
} SslInitStatus;

/**
 * Configure SSL
 *
 * NOTE: Must be called before starting the server if SSL mode is enabled
 * NOTE: Must be called again after each call to httpdShutdown()
 */
SslInitStatus httpdFreertosSslInit(HttpdFreertosInstance *pInstance);

/**
 * Set the ssl certificate and private key (in DER format)
 *
 * NOTE: Must be called before starting the server if SSL mode is enabled
 */
void httpdFreertosSslSetCertificateAndKey(HttpdFreertosInstance *pInstance,
                                        const void *certificate, size_t certificate_size,
                                        const void *private_key, size_t private_key_size);

typedef enum
{
    SslClientVerifyNone,
    SslClientVerifyRequired
} SslClientVerifySetting;

/**
 * Enable / disable client certificate verification
 *
 * NOTE: Ssl defaults to SslClientVerifyNone
 */
void httpdFreertosSslSetClientValidation(HttpdFreertosInstance *pInstance,
                                         SslClientVerifySetting verifySetting);

/**
 * Add a client certificate (in DER format)
 *
 * NOTE: Should use httpdFreertosSslSetClientValidation() to enable validation
 */
void httpdFreertosSslAddClientCertificate(HttpdFreertosInstance *pInstance,
                                          const void *certificate, size_t certificate_size);
#ifdef __cplusplus
} /* extern "C" */
#endif
