set (libesphttpd_SOURCES "core/auth.c"
                         "core/httpd-freertos.c"
                         "core/httpd.c"
                         "core/sha1.c"
                         "core/libesphttpd_base64.c"
                         "util/captdns.c"
                         "util/cgi_common.c"
                         "util/cgiflash.c"
                         "util/cgiredirect.c"
                         "util/cgiwebsocket.c"
                         "util/cgiredirect.c"
                         "util/esp32_httpd_vfs.c"
)

set (libesphttpd_PRIV_INCLUDE_DIRS "core"
                                   "util")

if (CONFIG_ESPHTTPD_USE_ESPFS)
    list (APPEND libesphttpd_SOURCES "core/httpd-espfs.c")
    list (APPEND libesphttpd_PRIV_INCLUDE_DIRS "../espfs/include")
endif (CONFIG_ESPHTTPD_USE_ESPFS)

idf_component_register(
    SRCS "${libesphttpd_SOURCES}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${libesphttpd_PRIV_INCLUDE_DIRS}"
    PRIV_REQUIRES "app_update"
             "json"
             "spi_flash"
             "openssl"
)

target_compile_definitions (${COMPONENT_TARGET} PUBLIC -DFREERTOS)
