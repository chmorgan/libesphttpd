set(libesphttpd_SRCS
  src/auth.c
  src/cgiredirect.c
  src/cgiwebsocket.c
  src/httpd-freertos.c
  src/httpd-vfs.c
  src/httpd.c
  src/libesphttpd_base64.c
  src/sha1.c)
set(libesphttpd_INCLUDE_DIRS
  include)
set(libesphttpd_PRIV_INCLUDE_DIRS
  src)
set(libesphttpd_REQUIRES)
set(libesphttpd_PRIV_REQUIRES)
  
if(ESP_PLATFORM)
  set(libesphttpd_SRCS ${libesphttpd_SRCS}
    src/captdns.c
    src/cgiwifi.c)
  set(libesphttpd_PRIV_REQUIRES ${libesphttpd_PRIV_REQUIRES}
    json)
endif()

if(CONFIG_IDF_TARGET_ESP32 OR
   CONFIG_IDF_TARGET_ESP32S2 OR
   CONFIG_IDF_TARGET_ESP32S3)
  set(libesphttpd_SRCS ${libesphttpd_SRCS}
    src/cgiflash.c
    src/esp32_flash.c
    src/esp32_wifi.c)
endif()

if(LINUX)
  set(libesphttpd_SRCS ${libesphttpd_SRCS}
    src/linux/esp_log.c)
  set (libesphttpd_PRIV_INCLUDE_DIRS ${libesphttpd_PRIV_INCLUDE_DIRS}
    src/linux)
endif()
