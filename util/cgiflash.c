/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Some flash handling cgi routines. Used for updating the ESPFS/OTA image.
*/

#include <libesphttpd/esp.h>
#include "libesphttpd/cgiflash.h"

#include "cJSON.h"

#include "httpd-platform.h"
#ifdef ESP32
#include "esp32_flash.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"

static const char *TAG = "ota";
#endif

#ifndef UPGRADE_FLAG_FINISH
#define UPGRADE_FLAG_FINISH     0x02
#endif

#define PARTITION_IS_FACTORY(partition) ((partition->type == ESP_PARTITION_TYPE_APP) && (partition->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY))
#define PARTITION_IS_OTA(partition) ((partition->type == ESP_PARTITION_TYPE_APP) && (partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN) && (partition->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX))


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
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
#ifdef ESP32
	//Doesn't matter, we have a MMU to remap memory, so we only have one firmware image.
	uint8_t id = 0;
#else
	uint8_t id = system_upgrade_userbin_check();
#endif
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "text/plain");
	httpdHeader(connData, "Content-Length", "9");
	httpdEndHeaders(connData);
	const char *next = id == 1 ? "user1.bin" : "user2.bin";
	httpdSend(connData, next, -1);
	ESP_LOGD(TAG, "Next firmware: %s (got %d)", next, id);
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
	const char *err;
} UploadState;

#ifndef ESP32
typedef struct __attribute__((packed)) {
	char magic[4];
	char tag[28];
	int32_t len1;
	int32_t len2;
} OtaHeader;
#endif

static void cgiJsonResponseCommon(HttpdConnData *connData, cJSON *jsroot){
	char *json_string = NULL;

	//// Generate the header
	//We want the header to start with HTTP code 200, which means the document is found.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Cache-Control", "no-store, must-revalidate, no-cache, max-age=0");
	httpdHeader(connData, "Expires", "Mon, 01 Jan 1990 00:00:00 GMT");  //  This one might be redundant, since modern browsers look for "Cache-Control".
	httpdHeader(connData, "Content-Type", "application/json; charset=utf-8"); //We are going to send some JSON.
	httpdEndHeaders(connData);
	json_string = cJSON_Print(jsroot);
    if (json_string)
    {
    	httpdSend(connData, json_string, -1);
        cJSON_free(json_string);
    }
    cJSON_Delete(jsroot);
}

#ifdef ESP32
CgiStatus ICACHE_FLASH_ATTR cgiUploadFirmware(HttpdConnData *connData) {
	CgiUploadFlashDef *def=(CgiUploadFlashDef*)connData->cgiArg;
	UploadState *state=(UploadState *)connData->cgiData;
	esp_err_t err;

	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		if (state!=NULL) free(state);
		return HTTPD_CGI_DONE;
	}

	if (state == NULL) {
		//First call. Allocate and initialize state variable.
		ESP_LOGD(TAG, "Firmware upload cgi start");
		state = malloc(sizeof(UploadState));
		if (state==NULL) {
			ESP_LOGE(TAG, "Can't allocate firmware upload struct");
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
		state->update_partition = NULL;
		// check arg partition name
		char arg_partition_buf[16] = "";
		int len;
//// HTTP GET queryParameter "partition" : string
	    len=httpdFindArg(connData->getArgs, "partition", arg_partition_buf, sizeof(arg_partition_buf));
	    if (len > 0)
	    {
	    	state->update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_ANY,arg_partition_buf);
	    }
	    else
	    {
	    	state->update_partition = esp_ota_get_next_update_partition(NULL);
	    }
	    if (state->update_partition == NULL)
	    {
			ESP_LOGE(TAG, "update_partition not found!");
			state->err="update_partition not found!";
			state->state=FLST_ERROR;
	    }

		connData->cgiData=state;
	}

	char *data = connData->post.buff;
	int dataLen = connData->post.buffLen;

	while (dataLen!=0) {
		if (state->state==FLST_START) {
			//First call. Assume the header of whatever we're uploading already is in the POST buffer.
			if (def->type==CGIFLASH_TYPE_FW && memcmp(data, "EHUG", 4)==0) {
				state->err="Combined flash images are unneeded/unsupported on ESP32!";
				state->state=FLST_ERROR;
				ESP_LOGE(TAG, "Combined flash image not supported on ESP32!");
			} else if (def->type==CGIFLASH_TYPE_FW && checkBinHeader(connData->post.buff)) {
			    if (state->update_partition == NULL)
			    {
					ESP_LOGE(TAG, "update_partition not found!");
					state->err="update_partition not found!";
					state->state=FLST_ERROR;
			    }
			    else
			    {
			    	ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x", state->update_partition->subtype, state->update_partition->address);

#ifdef CONFIG_ESPHTTPD_ALLOW_OTA_FACTORY_APP
					// hack the API to allow write to the factory partition!
					if (PARTITION_IS_FACTORY(state->update_partition))
					{
						esp_partition_subtype_t old_subtype = state->update_partition->subtype;
						esp_partition_subtype_t *pst = &(state->update_partition->subtype); // remove the const
						*pst = ESP_PARTITION_SUBTYPE_APP_OTA_MAX -1; // hack! set the type to an OTA to trick API into allowing write.
					err = esp_ota_begin(state->update_partition, OTA_SIZE_UNKNOWN, &state->update_handle);
						*pst = old_subtype; // put the value back to original now
					}
					else 
#endif
					{
						err = esp_ota_begin(state->update_partition, OTA_SIZE_UNKNOWN, &state->update_handle);
					}

					if (err != ESP_OK)
					{
						ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
						state->err="esp_ota_begin failed!";
						state->state=FLST_ERROR;
					}
					else
					{
						ESP_LOGI(TAG, "esp_ota_begin succeeded");
						state->state = FLST_WRITE;
						state->len = connData->post.len;
					}
			    }
			} else if (def->type==CGIFLASH_TYPE_ESPFS && checkEspfsHeader(connData->post.buff)) {
				if (connData->post.len > def->fwSize) {
					state->err="Firmware image too large";
					state->state=FLST_ERROR;
				} else {
					state->len=connData->post.len;
					state->address=def->fw1Pos;
					state->state=FLST_WRITE;
				}
			} else {
				state->err="Invalid flash image type!";
				state->state=FLST_ERROR;
				ESP_LOGE(TAG, "Did not recognize flash image type");
			}
		} else if (state->state==FLST_WRITE) {
			err = esp_ota_write(state->update_handle, data, dataLen);
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
				state->err="Error: esp_ota_write failed!";
				state->state=FLST_ERROR;
			}

			state->len-=dataLen;
			state->address+=dataLen;
			if (state->len==0) {
				state->state=FLST_DONE;
			}

			dataLen = 0;
		} else if (state->state==FLST_DONE) {
			ESP_LOGE(TAG, "%d bogus bytes received after data received", dataLen);
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

	if  (connData->post.len == connData->post.received) {
		cJSON *jsroot = cJSON_CreateObject();
		if (state->state==FLST_DONE) {
			if (esp_ota_end(state->update_handle) != ESP_OK) {
				state->err="esp_ota_end failed!";
		        ESP_LOGE(TAG, "esp_ota_end failed!");
		        state->state=FLST_ERROR;
		    }
			else
			{
				state->err="Flash Success.";
				ESP_LOGI(TAG, "Upload done. Sending response");
			}
			// todo: automatically set boot flag?
			err = esp_ota_set_boot_partition(state->update_partition);
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
			}
		}
		cJSON_AddStringToObject(jsroot, "message", state->err);
		cJSON_AddBoolToObject(jsroot, "success", (state->state==FLST_DONE)?true:false);
		free(state);

		cgiJsonResponseCommon(connData, jsroot); // Send the json response!
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

	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		if (state!=NULL) free(state);
		return HTTPD_CGI_DONE;
	}

	if (state==NULL) {
		//First call. Allocate and initialize state variable.
		ESP_LOGE(TAG, "Firmware upload cgi start");
		state=malloc(sizeof(UploadState));
		if (state==NULL) {
			ESP_LOGE(TAG, "Can't allocate firmware upload struct");
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
					ESP_LOGE(TAG, "OTA tag mismatch! Current=`%s` uploaded=`%s`",
										def->tagName, buff);
					len=httpdFindArg(connData->getArgs, "force", buff, sizeof(buff));
					if (len!=-1 && atoi(buff)) {
						ESP_LOGE(TAG, "Forcing firmware flash");
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
						ESP_LOGD(TAG, "Flashing user1.bin from ota image");
						state->len=h->len1;
						state->skip=h->len2;
						state->state=FLST_WRITE;
						state->address=def->fw1Pos;
					} else {
						ESP_LOGD(TAG, "Flashing user2.bin from ota image");
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
				ESP_LOGE(TAG, "Did not recognize flash image type");
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
			ESP_LOGE(TAG, "%d bogus bytes received after data received", dataLen);
			//Ignore those bytes.
			dataLen=0;
		} else if (state->state==FLST_ERROR) {
			//Just eat up any bytes we receive.
			dataLen=0;
		}
	}

	if (connData->post->len==connData->post->received) {
		//We're done! Format a response.
		ESP_LOGD(TAG, "Upload done. Sending response");
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
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	cJSON *jsroot = cJSON_CreateObject();

	ESP_LOGD(TAG, "Reboot Command recvd. Sending response");
	// TODO: sanity-check that the 'next' partition actually contains something that looks like
	// valid firmware

	//Do reboot in a timer callback so we still have time to send the response.
	resetTimer=httpdPlatTimerCreate("flashreset", 500, 0, resetTimerCb, NULL);
	httpdPlatTimerStart(resetTimer);

	cJSON_AddStringToObject(jsroot, "message", "Rebooting...");
	cJSON_AddBoolToObject(jsroot, "success", true);
	cgiJsonResponseCommon(connData, jsroot); // Send the json response!
	return HTTPD_CGI_DONE;
}

// Handle request to set boot flag
CgiStatus ICACHE_FLASH_ATTR cgiSetBoot(HttpdConnData *connData) {
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	const esp_partition_t *wanted_bootpart = NULL;
	const esp_partition_t *actual_bootpart = NULL;
	cJSON *jsroot = cJSON_CreateObject();

	// check arg partition name
	char arg_partition_buf[16] = "";
	int len;
//// HTTP GET queryParameter "partition" : string
    len=httpdFindArg(connData->getArgs, "partition", arg_partition_buf, sizeof(arg_partition_buf));
    if (len > 0)
    {
    	ESP_LOGD(TAG, "Set Boot Command recvd. for partition with name: %s", arg_partition_buf);
    	wanted_bootpart = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_ANY,arg_partition_buf);
    	esp_err_t err = esp_ota_set_boot_partition(wanted_bootpart);
    	if (err != ESP_OK) {
    		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
    	}
    }
    actual_bootpart = esp_ota_get_boot_partition(); // if above failed or arg not specified, return what is currently set for boot.

	cJSON_AddStringToObject(jsroot, "boot", actual_bootpart->label);
	cJSON_AddBoolToObject(jsroot, "success", (wanted_bootpart == NULL || wanted_bootpart == actual_bootpart));

	cgiJsonResponseCommon(connData, jsroot); // Send the json response!
	return HTTPD_CGI_DONE;
}

// Handle request to format a data partition
CgiStatus ICACHE_FLASH_ATTR cgiEraseFlash(HttpdConnData *connData) {
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	const esp_partition_t *wanted_partition = NULL;
	cJSON *jsroot = cJSON_CreateObject();
	esp_err_t err = ESP_FAIL;

	// check arg partition name
	char arg_partition_buf[16] = "";
	int len;
//// HTTP GET queryParameter "partition" : string
    len=httpdFindArg(connData->getArgs, "partition", arg_partition_buf, sizeof(arg_partition_buf));
    if (len > 0)
    {
    	ESP_LOGD(TAG, "Erase command recvd. for partition with name: %s", arg_partition_buf);
    	wanted_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,arg_partition_buf);
    	err = esp_partition_erase_range(wanted_partition, 0, wanted_partition->size);
    	if (err != ESP_OK) {
    		ESP_LOGE(TAG, "erase partition failed! err=0x%x", err);
    	}
    	else
    	{
    		ESP_LOGW(TAG, "Data partition: %s is erased now!  Must reboot to reformat it!", wanted_partition->label);
    		cJSON_AddStringToObject(jsroot, "erased", wanted_partition->label);
    	}
    }

	cJSON_AddBoolToObject(jsroot, "success", (err == ESP_OK));

	cgiJsonResponseCommon(connData, jsroot); // Send the json response!
	return HTTPD_CGI_DONE;
}

/* @brief Check if selected partition has a valid APP
 *  Warning- this takes a long time to execute and dumps a bunch of stuff to the console!
 *  todo: find a faster method to veryify an APP
 */
static int check_partition_valid_app(const esp_partition_t *partition)
{
    if (partition == NULL) {
        return 0;
    }

    esp_image_metadata_t data;
    const esp_partition_pos_t part_pos = {
        .offset = partition->address,
        .size = partition->size,
    };
	if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &part_pos, &data) != ESP_OK) {
        return 0;  // partition does not hold a valid app
    }
    return 1; // App in partition is valid
}

// Cgi to query info about partitions and firmware
CgiStatus ICACHE_FLASH_ATTR cgiGetFlashInfo(HttpdConnData *connData) {
	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	const esp_partition_t *running_partition = NULL;
	const esp_partition_t *boot_partition = NULL;

	cJSON *jsroot = cJSON_CreateObject();
	// check arg
	char arg_1_buf[16] = "";
	int len;
//// HTTP GET queryParameter "ptype" : string ("app", "data")
	bool get_app = true;  // get both app and data partitions by default
	bool get_data = true;
    len=httpdFindArg(connData->getArgs, "ptype", arg_1_buf, sizeof(arg_1_buf));
    if (len > 0)
    {
    	if (strcmp(arg_1_buf, "app") == 0)
    	{
    		get_data = false;  // don't get data partitinos if client specified ?type=app
    	}
    	else if (strcmp(arg_1_buf, "data") == 0)
    	{
    		get_app = false;  // don't get app partitinos if client specified ?type=data
    	}
    }
//// HTTP GET queryParameter "verify"	: number 0,1
	bool verify_app = false;  // default don't verfiy apps, because it takes a long time.
    len=httpdFindArg(connData->getArgs, "verify", arg_1_buf, sizeof(arg_1_buf));
    if (len > 0) {
    	char ch;  // dummy to test for malformed input
    	int val;
    	int n = sscanf(arg_1_buf, "%d%c", &val, &ch);
		if (n == 1) {
			/* sscanf found a number to convert */
			verify_app = (val == 1)?(true):(false);
		}
    }
//// HTTP GET queryParameter "partition" : string
	bool specify_partname = false;
	len=httpdFindArg(connData->getArgs, "partition", arg_1_buf, sizeof(arg_1_buf));
	if (len > 0)
	{
		specify_partname = true;
	}

    if (get_app)
    {
    	running_partition = esp_ota_get_running_partition();
    	boot_partition = esp_ota_get_boot_partition();
    	if (boot_partition == NULL) {boot_partition = running_partition;} // If no ota_data partition, then esp_ota_get_boot_partition() might return NULL.
    	cJSON *jsapps = cJSON_AddArrayToObject(jsroot, "app");
    	esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, (specify_partname)?(arg_1_buf):(NULL));
        while (it != NULL) {
            const esp_partition_t *it_partition = esp_partition_get(it);
    		if (it_partition != NULL)
    		{
    			cJSON *partj = NULL;
    			partj = cJSON_CreateObject();
    			cJSON_AddStringToObject(partj, "name", it_partition->label);
    			cJSON_AddNumberToObject(partj, "size", it_partition->size);

                // Note esp_ota_get_partition_description() was introduced in ESP-IDF 3.3.
#ifdef ESP_APP_DESC_MAGIC_WORD // enable functionality only if present in IDF by testing for macro.
                esp_app_desc_t app_info;
                if (esp_ota_get_partition_description(it_partition, &app_info) == ESP_OK)
                {
                    cJSON_AddStringToObject(partj, "project_name", app_info.project_name);
                    cJSON_AddStringToObject(partj, "version", app_info.version);
                }
                else
#endif
                {
                    cJSON_AddStringToObject(partj, "version", "");
                }
#ifdef CONFIG_ESPHTTPD_ALLOW_OTA_FACTORY_APP
				cJSON_AddBoolToObject(partj, "ota", true);
#else
    			cJSON_AddBoolToObject(partj, "ota", PARTITION_IS_OTA(it_partition));
#endif
    			if (verify_app)
    			{
    				cJSON_AddBoolToObject(partj, "valid", check_partition_valid_app(it_partition));
    			}
    			cJSON_AddBoolToObject(partj, "running", (it_partition==running_partition));
    			cJSON_AddBoolToObject(partj, "bootset", (it_partition==boot_partition));
    			cJSON_AddItemToArray(jsapps, partj);
    			it = esp_partition_next(it);
    		}
        }
    	esp_partition_iterator_release(it);
    }

	if (get_data)
	{
		cJSON *jsdatas = cJSON_AddArrayToObject(jsroot, "data");
		esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, (specify_partname)?(arg_1_buf):(NULL));
	    while (it != NULL) {
	        const esp_partition_t *it_partition = esp_partition_get(it);
			if (it_partition != NULL)
			{
				cJSON *partj = NULL;
				partj = cJSON_CreateObject();
				cJSON_AddStringToObject(partj, "name", it_partition->label);
				cJSON_AddNumberToObject(partj, "size", it_partition->size);
				cJSON_AddNumberToObject(partj, "format", it_partition->subtype);
				cJSON_AddItemToArray(jsdatas, partj);
				it = esp_partition_next(it);
			}
	    }
		esp_partition_iterator_release(it);
	}
	cJSON_AddBoolToObject(jsroot, "success", true);
	cgiJsonResponseCommon(connData, jsroot); // Send the json response!
	return HTTPD_CGI_DONE;
}
