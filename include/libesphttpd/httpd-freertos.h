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

/**
 * Manually execute webserver task - do not use with httpdFreertosStart
 */
PLAT_RETURN platHttpServerTask(void *pvParameters);

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
