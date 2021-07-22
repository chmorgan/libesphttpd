/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Routines for handling the (slightly more complicated) esp32 flash.
Broken out because esp-idf is expected to get better routines for this.
*/

#include <libesphttpd/esp.h>
#ifdef ESP32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp32/rom/cache.h"
#include "esp32/rom/ets_sys.h"
#include "esp32/rom/spi_flash.h"
#include "esp32/rom/crc.h"
#include "esp32/rom/rtc.h"
#include "esp_partition.h"

/*   Size of 32 bytes is friendly to flash encryption */
typedef struct {
    uint32_t ota_seq;
    uint8_t  seq_label[24];
    uint32_t crc; /* CRC32 of ota_seq field only */
} ota_select;


static uint32_t ota_select_crc(const ota_select *s)
{
  return crc32_le(UINT32_MAX, (uint8_t*)&s->ota_seq, 4);
}

static bool ota_select_valid(const ota_select *s)
{
  return s->ota_seq != UINT32_MAX && s->crc == ota_select_crc(s);
}

//ToDo: Allow more OTA partitions than the current 2
static int getOtaSel() {
	int selectedPart;
	ota_select sa1, sa2;
	const esp_partition_t* otaselpart=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
	spi_flash_read((uint32)otaselpart->address, (uint32_t*)&sa1, sizeof(ota_select));
	spi_flash_read((uint32)otaselpart->address+0x1000, (uint32_t*)&sa2, sizeof(ota_select));
	if (ota_select_valid(&sa1) && ota_select_valid(&sa2)) {
		selectedPart=(((sa1.ota_seq > sa2.ota_seq)?sa1.ota_seq:sa2.ota_seq))%2;
	} else if (ota_select_valid(&sa1)) {
		selectedPart=(sa1.ota_seq)%2;
	} else if (ota_select_valid(&sa2)) {
		selectedPart=(sa2.ota_seq)%2;
	} else {
		printf("esp32 ota: no valid ota select sector found!\n");
		selectedPart=-1;
	}
	printf("OTA part select ID: %d\n", selectedPart);
	return selectedPart;
}


int esp32flashGetUpdateMem(uint32_t *loc, uint32_t *size) {
	const esp_partition_t* otaselpart;
	int selectedPart=getOtaSel();
	if (selectedPart==-1) return 0;
	otaselpart=esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0+selectedPart, NULL);
	*loc=otaselpart->address;
	*size=otaselpart->size;
	return 1;
}


int esp32flashSetOtaAsCurrentImage() {
	const esp_partition_t* otaselpart=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
	int selSect=-1;
	ota_select sa1,sa2, newsa;
	spi_flash_read((uint32)otaselpart->address, (uint32_t*)&sa1, sizeof(ota_select));
	spi_flash_read((uint32)otaselpart->address+0x1000, (uint32_t*)&sa2, sizeof(ota_select));
	if (ota_select_valid(&sa1) && ota_select_valid(&sa2)) {
		selSect=(sa1.ota_seq > sa2.ota_seq)?1:0;
	} else if (ota_select_valid(&sa1)) {
		selSect=1;
	} else if (ota_select_valid(&sa2)) {
		selSect=0;
	} else {
		printf("esp32 ota: no valid ota select sector found!\n");
	}
	if (selSect==0) {
		newsa.ota_seq=sa2.ota_seq+1;
		printf("Writing seq %d to ota select sector 1\n", newsa.ota_seq);
		newsa.crc = ota_select_crc(&newsa);
		spi_flash_erase_sector(otaselpart->address/0x1000);
		spi_flash_write(otaselpart->address,(uint32_t *)&newsa,sizeof(ota_select));
	} else {
		printf("Writing seq %d to ota select sector 2\n", newsa.ota_seq);
		newsa.ota_seq=sa1.ota_seq+1;
		newsa.crc = ota_select_crc(&newsa);
		spi_flash_erase_sector(otaselpart->address/0x1000+1);
		spi_flash_write(otaselpart->address+0x1000,(uint32_t *)&newsa,sizeof(ota_select));
	}
	return 1;
}

int esp32flashRebootIntoOta() {
	software_reset();
	return 1;
}


#endif
