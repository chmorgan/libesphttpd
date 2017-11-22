/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Some flash handling cgi routines. Used for updating the ESPFS/OTA image.
*/

#include <libesphttpd/esp.h>
#include "libesphttpd/cgiflash.h"
#include "libesphttpd/espfs.h"
#include "libesphttpd/cgiflash.h"
#include "libesphttpd/espfs.h"

//#include <osapi.h>
#include "libesphttpd/cgiflash.h"
#include "libesphttpd/espfs.h"
#include "httpd-platform.h"
#ifdef ESP32
#include "esp32_flash.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "ota";
#endif

#ifndef UPGRADE_FLAG_FINISH
#define UPGRADE_FLAG_FINISH     0x02
#endif

// Check that the header of the firmware blob looks like actual firmware...
static int ICACHE_FLASH_ATTR checkBinHeader(void *buf) {
	uint8_t *cd = (uint8_t *)buf;
#ifdef ESP32
	printf("checkBinHeader: %x %x %x\n", cd[0], ((uint16_t *)buf)[3], ((uint32_t *)buf)[0x6]);
	if (cd[0] != 0xE9) return 0;
	if (((uint16_t *)buf)[3] != 0x4008) return 0;
	uint32_t a=((uint32_t *)buf)[0x6];
	if (a!=0 && (a<=0x3F000000 || a>0x40400000)) return 0;
#else
	if (cd[0] != 0xEA) return 0;
	if (cd[1] != 4 || cd[2] > 3 || cd[3] > 0x40) return 0;
	if (((uint16_t *)buf)[3] != 0x4010) return 0;
	if (((uint32_t *)buf)[2] != 0) return 0;
#endif
	return 1;
}

static int ICACHE_FLASH_ATTR checkEspfsHeader(void *buf) {
	if (memcmp(buf, "ESfs", 4)!=0) return 0;
	return 1;
}


// Cgi to query which firmware needs to be uploaded next
CgiStatus ICACHE_FLASH_ATTR cgiGetFirmwareNext(HttpdConnData *connData) {
	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
#ifdef ESP32
	//Doesn't matter, we have a MMU to remap memory, so we only have one firmware image.
	uint8 id = 0;
#else
	uint8 id = system_upgrade_userbin_check();
#endif
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "text/plain");
	httpdHeader(connData, "Content-Length", "9");
	httpdEndHeaders(connData);
	char *next = id == 1 ? "user1.bin" : "user2.bin";
	httpdSend(connData, next, -1);
	httpd_printf("Next firmware: %s (got %d)\n", next, id);
	return HTTPD_CGI_DONE;
}


//Cgi that allows the firmware to be replaced via http POST This takes
//a direct POST from e.g. Curl or a Javascript AJAX call with either the
//firmware given by cgiGetFirmwareNext or an OTA upgrade image.

//Because we don't have the buffer to allocate an entire sector but will
//have to buffer some data because the post buffer may be misaligned, we
//write SPI data in pages. The page size is a software thing, not
//a hardware one.
#ifdef ESP32
#define PAGELEN 4096
#else
#define PAGELEN 64
#endif

#define FLST_START 0
#define FLST_WRITE 1
#ifndef ESP32
#define FLST_SKIP 2
#endif
#define FLST_DONE 3
#define FLST_ERROR 4

#define FILETYPE_ESPFS 0
#define FILETYPE_FLASH 1
#define FILETYPE_OTA 2
typedef struct {
#ifdef ESP32
	esp_ota_handle_t update_handle;
	const esp_partition_t *update_partition;
	const esp_partition_t *configured;
	const esp_partition_t *running;
#endif
	int state;
	int filetype;
	int flashPos;
	char pageData[PAGELEN];
#ifndef ESP32
	int pagePos;
#endif
	int address;
	int len;
	int skip;
	char *err;
} UploadState;

#ifndef ESP32
typedef struct __attribute__((packed)) {
	char magic[4];
	char tag[28];
	int32_t len1;
	int32_t len2;
} OtaHeader;
#endif

#ifdef ESP32
CgiStatus ICACHE_FLASH_ATTR cgiUploadFirmware(HttpdConnData *connData) {
	CgiUploadFlashDef *def=(CgiUploadFlashDef*)connData->cgiArg;
	UploadState *state=(UploadState *)connData->cgiData;
	esp_err_t err;

	if (connData->conn == NULL) {
		//Connection aborted. Clean up.
		if (state!=NULL) free(state);
		return HTTPD_CGI_DONE;
	}

	if (state == NULL) {
		//First call. Allocate and initialize state variable.
		httpd_printf("Firmware upload cgi start.\n");
		state = malloc(sizeof(UploadState));
		if (state==NULL) {
			httpd_printf("Can't allocate firmware upload struct!\n");
			return HTTPD_CGI_DONE;
		}
		memset(state, 0, sizeof(UploadState));

		state->configured = esp_ota_get_boot_partition();
		state->running = esp_ota_get_running_partition();

		// check that ota support is enabled
		if(!state->configured || !state->running)
		{
			ESP_LOGE(TAG, "configured or running parititon is null, is OTA support enabled in build configuration?");
			state->state=FLST_ERROR;
			state->err="Partition error, OTA not supported?";
		} else {
			if (state->configured != state->running) {
				ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
					state->configured->address, state->running->address);
				ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
			}
			ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
				state->running->type, state->running->subtype, state->running->address);

			state->state=FLST_START;
			state->err="Premature end";
		}

		connData->cgiData=state;
	}

	char *data = connData->post->buff;
	int dataLen = connData->post->buffLen;

	while (dataLen!=0) {
		if (state->state==FLST_START) {
			//First call. Assume the header of whatever we're uploading already is in the POST buffer.
			if (def->type==CGIFLASH_TYPE_FW && memcmp(data, "EHUG", 4)==0) {
				state->err="Combined flash images are unneeded/unsupported on ESP32!";
				state->state=FLST_ERROR;
				httpd_printf("Combined flash image not supported on ESP32!\n");
			} else if (def->type==CGIFLASH_TYPE_FW && checkBinHeader(connData->post->buff)) {
				state->update_partition = esp_ota_get_next_update_partition(NULL);
				ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
					state->update_partition->subtype, state->update_partition->address);
				assert(state->update_partition != NULL);

				err = esp_ota_begin(state->update_partition, OTA_SIZE_UNKNOWN, &state->update_handle);
				if (err != ESP_OK) {
					ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
				}
				ESP_LOGI(TAG, "esp_ota_begin succeeded");

				state->state = FLST_WRITE;
				state->len = connData->post->len;
			} else if (def->type==CGIFLASH_TYPE_ESPFS && checkEspfsHeader(connData->post->buff)) {
				if (connData->post->len > def->fwSize) {
					state->err="Firmware image too large";
					state->state=FLST_ERROR;
				} else {
					state->len=connData->post->len;
					state->address=def->fw1Pos;
					state->state=FLST_WRITE;
				}
			} else {
				state->err="Invalid flash image type!";
				state->state=FLST_ERROR;
				httpd_printf("Did not recognize flash image type!\n");
			}
		} else if (state->state==FLST_WRITE) {
			err = esp_ota_write(state->update_handle, data, dataLen);
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
			}

			state->len-=dataLen;
			state->address+=dataLen;
			if (state->len==0) {
				state->state=FLST_DONE;
			}

			dataLen = 0;
		} else if (state->state==FLST_DONE) {
			httpd_printf("Huh? %d bogus bytes received after data received.\n", dataLen);
			//Ignore those bytes.
			dataLen=0;
		} else if (state->state==FLST_ERROR) {
			//Just eat up any bytes we receive.
			dataLen=0;
		}
	}

#if 0
	//TODO: maybe use ESP_LOGD() here in the future
	printf("post->len %d, post->received %d\n", connData->post->len,
		connData->post->received);
	printf("state->len %d, state->address: %d\n", state->len, state->address);
#endif

	if (connData->post->len == connData->post->received) {
		//We're done! Format a response.
		httpd_printf("Upload done. Sending response.\n");
		httpdStartResponse(connData, state->state==FLST_ERROR?400:200);
		httpdHeader(connData, "Content-Type", "text/plain");
		httpdEndHeaders(connData);
		if (state->state!=FLST_DONE) {
			httpdSend(connData, "Firmware image error:", -1);
			httpdSend(connData, state->err, -1);
			httpdSend(connData, "\n", -1);
		} else {
			if (esp_ota_end(state->update_handle) != ESP_OK) {
		        ESP_LOGE(TAG, "esp_ota_end failed!");
		    }
		    err = esp_ota_set_boot_partition(state->update_partition);
		    if (err != ESP_OK) {
		        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
		    }
		}
		free(state);
		return HTTPD_CGI_DONE;
	}

	return HTTPD_CGI_MORE;
}

#else

CgiStatus ICACHE_FLASH_ATTR cgiUploadFirmware(HttpdConnData *connData) {
	CgiUploadFlashDef *def=(CgiUploadFlashDef*)connData->cgiArg;
	UploadState *state=(UploadState *)connData->cgiData;
	int len;
	char buff[128];

	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		if (state!=NULL) free(state);
		return HTTPD_CGI_DONE;
	}

	if (state==NULL) {
		//First call. Allocate and initialize state variable.
		httpd_printf("Firmware upload cgi start.\n");
		state=malloc(sizeof(UploadState));
		if (state==NULL) {
			httpd_printf("Can't allocate firmware upload struct!\n");
			return HTTPD_CGI_DONE;
		}
		memset(state, 0, sizeof(UploadState));
		state->state=FLST_START;
		connData->cgiData=state;
		state->err="Premature end";
	}

	char *data=connData->post->buff;
	int dataLen=connData->post->buffLen;

	while (dataLen!=0) {
		if (state->state==FLST_START) {
			//First call. Assume the header of whatever we're uploading already is in the POST buffer.
			if (def->type==CGIFLASH_TYPE_FW && memcmp(data, "EHUG", 4)==0) {
				//Type is combined flash1/flash2 file
				OtaHeader *h=(OtaHeader*)data;
				strncpy(buff, h->tag, 27);
				buff[27]=0;
				if (strcmp(buff, def->tagName)!=0) {
					httpd_printf("OTA tag mismatch! Current=`%s` uploaded=`%s`.\n",
										def->tagName, buff);
					len=httpdFindArg(connData->getArgs, "force", buff, sizeof(buff));
					if (len!=-1 && atoi(buff)) {
						httpd_printf("Forcing firmware flash.\n");
					} else {
						state->err="Firmware not intended for this device!\n";
						state->state=FLST_ERROR;
					}
				}
				if (state->state!=FLST_ERROR && connData->post->len > def->fwSize*2+sizeof(OtaHeader)) {
					state->err="Firmware image too large";
					state->state=FLST_ERROR;
				}
				if (state->state!=FLST_ERROR) {
					//Flash header seems okay.
					dataLen-=sizeof(OtaHeader); //skip header when parsing data
					data+=sizeof(OtaHeader);
					if (system_upgrade_userbin_check()==1) {
						httpd_printf("Flashing user1.bin from ota image\n");
						state->len=h->len1;
						state->skip=h->len2;
						state->state=FLST_WRITE;
						state->address=def->fw1Pos;
					} else {
						httpd_printf("Flashing user2.bin from ota image\n");
						state->len=h->len2;
						state->skip=h->len1;
						state->state=FLST_SKIP;
						state->address=def->fw2Pos;
					}
				}
			} else if (def->type==CGIFLASH_TYPE_FW && checkBinHeader(connData->post->buff)) {
				if (connData->post->len > def->fwSize) {
					state->err="Firmware image too large";
					state->state=FLST_ERROR;
				} else {
					state->len=connData->post->len;
					state->address=def->fw1Pos;
					state->state=FLST_WRITE;
				}
			} else if (def->type==CGIFLASH_TYPE_ESPFS && checkEspfsHeader(connData->post->buff)) {
				if (connData->post->len > def->fwSize) {
					state->err="Firmware image too large";
					state->state=FLST_ERROR;
				} else {
					state->len=connData->post->len;
					state->address=def->fw1Pos;
					state->state=FLST_WRITE;
				}
			} else {
				state->err="Invalid flash image type!";
				state->state=FLST_ERROR;
				httpd_printf("Did not recognize flash image type!\n");
			}
		} else if (state->state==FLST_SKIP) {
			//Skip bytes without doing anything with them
			if (state->skip>dataLen) {
				//Skip entire buffer
				state->skip-=dataLen;
				dataLen=0;
			} else {
				//Only skip part of buffer
				dataLen-=state->skip;
				data+=state->skip;
				state->skip=0;
				if (state->len) state->state=FLST_WRITE; else state->state=FLST_DONE;
			}
		} else if (state->state==FLST_WRITE) {
			//Copy bytes to page buffer, and if page buffer is full, flash the data.
			//First, calculate the amount of bytes we need to finish the page buffer.
			int lenLeft=PAGELEN-state->pagePos;
			if (state->len<lenLeft) lenLeft=state->len; //last buffer can be a cut-off one
			//See if we need to write the page.
			if (dataLen<lenLeft) {
				//Page isn't done yet. Copy data to buffer and exit.
				memcpy(&state->pageData[state->pagePos], data, dataLen);
				state->pagePos+=dataLen;
				state->len-=dataLen;
				dataLen=0;
			} else {
				//Finish page; take data we need from post buffer
				memcpy(&state->pageData[state->pagePos], data, lenLeft);
				data+=lenLeft;
				dataLen-=lenLeft;
				state->pagePos+=lenLeft;
				state->len-=lenLeft;
				//Erase sector, if needed
				if ((state->address&(SPI_FLASH_SEC_SIZE-1))==0) {
					spi_flash_erase_sector(state->address/SPI_FLASH_SEC_SIZE);
				}
				//Write page
				//httpd_printf("Writing %d bytes of data to SPI pos 0x%x...\n", state->pagePos, state->address);
				spi_flash_write(state->address, (uint32 *)state->pageData, state->pagePos);
				state->address+=PAGELEN;
				state->pagePos=0;
				if (state->len==0) {
					//Done.
					if (state->skip) state->state=FLST_SKIP; else state->state=FLST_DONE;
				}
			}
		} else if (state->state==FLST_DONE) {
			httpd_printf("Huh? %d bogus bytes received after data received.\n", dataLen);
			//Ignore those bytes.
			dataLen=0;
		} else if (state->state==FLST_ERROR) {
			//Just eat up any bytes we receive.
			dataLen=0;
		}
	}

	if (connData->post->len==connData->post->received) {
		//We're done! Format a response.
		httpd_printf("Upload done. Sending response.\n");
		httpdStartResponse(connData, state->state==FLST_ERROR?400:200);
		httpdHeader(connData, "Content-Type", "text/plain");
		httpdEndHeaders(connData);
		if (state->state!=FLST_DONE) {
			httpdSend(connData, "Firmware image error:", -1);
			httpdSend(connData, state->err, -1);
			httpdSend(connData, "\n", -1);
		}
		free(state);
		return HTTPD_CGI_DONE;
	}

	return HTTPD_CGI_MORE;
}
#endif

static HttpdPlatTimerHandle resetTimer;

static void ICACHE_FLASH_ATTR resetTimerCb(void *arg) {
#ifndef ESP32
	system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
	system_upgrade_reboot();
#else
	esp32flashRebootIntoOta();
#endif
}

// Handle request to reboot into the new firmware
CgiStatus ICACHE_FLASH_ATTR cgiRebootFirmware(HttpdConnData *connData) {
	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}

	// TODO: sanity-check that the 'next' partition actually contains something that looks like
	// valid firmware

	//Do reboot in a timer callback so we still have time to send the response.
	resetTimer=httpdPlatTimerCreate("flashreset", 200, 0, resetTimerCb, NULL);
	httpdPlatTimerStart(resetTimer);

	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "text/plain");
	httpdEndHeaders(connData);
	httpdSend(connData, "Rebooting...", -1);
	return HTTPD_CGI_DONE;
}
