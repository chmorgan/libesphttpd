#ifndef CGIFLASH_H
#define CGIFLASH_H

#include "httpd.h"

#define CGIFLASH_TYPE_FW 0
#define CGIFLASH_TYPE_ESPFS 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const void *update_partition; // maybe useful info for the callback. Points to a esp_partition_t
  int totalLen; // total length of the image in bytes
  int received; // how much has been received so far
} CgiUploadFlashStatus;

typedef void(*CgiFlashUploadCb)(const CgiUploadFlashStatus *status);

typedef struct {
	int type; // CGIFLASH_TYPE_FW or CGIFLASH_TYPE_ESPFS
	int fw1Pos; // not used for ESP32 FW
	int fw2Pos; // not used for ESP32 FW
	int fwSize; // not used for ESP32 FW
	const char *tagName; // not used for ESP32
	CgiFlashUploadCb beforeBeginCb; // Optional callback to be called before flash writing begins (i.e. shutdown unessary tasks)
	CgiFlashUploadCb progressCb; // Optional callback to be called when upload progresses
	CgiFlashUploadCb endCb; // Optional callback to be called after flash writing ends
} CgiUploadFlashDef;

CgiStatus cgiGetFirmwareNext(HttpdConnData *connData);
CgiStatus cgiUploadFirmware(HttpdConnData *connData);
CgiStatus cgiRebootFirmware(HttpdConnData *connData);
CgiStatus cgiSetBoot(HttpdConnData *connData);
CgiStatus cgiEraseFlash(HttpdConnData *connData);
CgiStatus cgiGetFlashInfo(HttpdConnData *connData);

#ifdef __cplusplus
}
#endif
#endif
