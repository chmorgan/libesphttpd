#pragma once

#include <stdint.h>


int esp32flashGetUpdateMem(uint32_t *loc, uint32_t *size);
int esp32flashSetOtaAsCurrentImage();
int esp32flashRebootIntoOta();
