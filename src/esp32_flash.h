#ifndef ESP32_FLASH_H
#define ESP32_FLASH_H
int esp32flashGetUpdateMem(uint32_t *loc, uint32_t *size);
int esp32flashSetOtaAsCurrentImage();
int esp32flashRebootIntoOta();
#endif