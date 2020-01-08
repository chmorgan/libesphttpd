# Libesphttpd Flash-API

Functions to flash firmware Over-The-Air.  These are only useful if you have enabled OTA support.

The simplest way to use the partition table is to make menuconfig and choose the simple predefined partition table:

    “Factory app, two OTA definitions”

## GUI
See the example js/html code for the GUI here: https://github.com/chmorgan/esphttpd-freertos/blob/master/html/flash/index.html

## Functions defined in cgiflash.h

* __cgiGetFirmwareNext()__
Legacy function for ESP8266 (not needed for ESP32)

* __cgiUploadFirmware()__
CGI function writes HTTP POST data to flash.

* __cgiRebootFirmware()__
CGI function reboots the ESP firmware after a short time-delay.

* __cgiSetBoot()__
CGI function to change the selected boot partition.

* __cgiEraseFlash()__
CGI function to erase flash memory.  (only supports erasing data partitions)

* __cgiGetFlashInfo()__
CGI function returns a JSON object describing the partition table.  It can also verify the firmware images, but not by default because that process takes several seconds.

## Configuration Options

* __Allow OTA of Factory Partition (not recommended)__
Defines CONFIG_ESPHTTPD_ALLOW_OTA_FACTORY_APP.  This option allows cgiUploadFirmware() (in cgiflash.c) to write to the Factory partition.  It it not recommended for production use.  It is useful if you need to update the factory app via OTA for some reason.  To use it, build this project with CONFIG_ESPHTTPD_ALLOW_OTA_FACTORY_APP and upload it to one of the OTA partitions on your device.  Then reboot your device into that OTA and then you will be able to upload an image to the factory partition.

## HTTP REST API

The flash API is specified in RAML.  (see https://raml.org)

```yaml
#%RAML 1.0
title: flash
version: v1
baseUri: http://me.local/flash

/flashinfo.json:
    description: Flash info
    get:
        description: Returns a JSON of info about the partition table
        queryParameters:
            ptype:
                displayName: ptype
                type: string
                description: String name of partition type (app, data).  If not specified, return both app and data partitions.
                example: app
                required: false
            verify:
                displayName: verify
                type: number
                description: 1: verify the app partitions.  0 (default): don't verify.  Note: verification takes >2s per partition!
                example: 1
                required: false
            partition:
                displayName: partition
                type: string
                description: String name of partition (i.e. factory, ota_0, ota_1).  If not specified, return all.
                example: ota_0
                required: false
        responses:
            200:
                body:
                    application/json:
                      type: object
                      properties:
                        app: array
                        data: array
                      example: |
                        {
                            "app":	[{
                                    "name":	"factory",
                                    "size":	4259840,
                                    "version":	"",
                                    "ota":	false,
                                    "valid":	true,
                                    "running":	false,
                                    "bootset":	false
                                }, {
                                    "name":	"ota_0",
                                    "size":	4259840,
                                    "version":	"",
                                    "ota":	true,
                                    "valid":	true,
                                    "running":	true,
                                    "bootset":	true
                                }, {
                                    "name":	"ota_1",
                                    "size":	4259840,
                                    "version":	"",
                                    "ota":	true,
                                    "valid":	true,
                                    "running":	false,
                                    "bootset":	false
                                }],
                            "data":	[{
                                    "name":	"nvs",
                                    "size":	16384,
                                    "format":	2
                                }, {
                                    "name":	"otadata",
                                    "size":	8192,
                                    "format":	0
                                }, {
                                    "name":	"phy_init",
                                    "size":	4096,
                                    "format":	1
                                }, {
                                    "name":	"internalfs",
                                    "size":	3932160,
                                    "format":	129
                                }]
                        }

/setboot:
    description: boot flag
    get:
        description: Set the boot flag.  example GET /flash/setboot?partition=ota_1
        queryParameters:
            partition:
                displayName: partition
                type: string
                description: String name of partition (i.e. factory, ota_0, ota_1).  If not specified, just return the current setting.
                example: ota_0
                required: false
        responses:
            200:
                body:
                    application/json:
                      type: object
                      properties:
                        success: boolean
                        boot: string
                      example: |
                        {
                          "success": true
                          "boot": "ota_0"
                        }
                        
/reboot:
    description: Reboot the processor
    get:
        description: Reboot the processor.  Waits 0.5s before rebooting to allow the JSON response to be sent.
        responses:
            200:
                body:
                    application/json:
                      type: object
                      properties:
                        success: boolean
                        message: string
                      example: |
                        {
                          "success": true
                          "message": "Rebooting..."
                        }
                        
/upload:
    description: Upload APP
    post:
        description: Upload APP firmware to flash memory.
        queryParameters:
            partition:
                displayName: partition
                type: string
                description: String name of partition (i.e. factory, ota_0, ota_1).  If not specified, automatically choose the next available OTA slot.
                example: ota_0
                required: false
        responses:
            200:
                body:
                    application/json:
                      type: object
                      properties:
                        success: boolean
                        message: string
                      example: |
                          {
                              "success": true
                              "message": "Flash Success."
                          }
                          
/erase:
    description: Erase data
    get:
        description: Erase data in a data (not app) partition.  Recommend reboot immediately after to force a reformat of the data.
        queryParameters:
            partition:
                displayName: partition
                type: string
                description: String name of data partition (i.e. internalfs, nvs, ota_data).  If not specified, nothing is erased.
                example: internalfs
                required: true
        responses:
            200:
                body:
                    application/json:
                      type: object
                      properties:
                        success: boolean
                        message: string
                      example: |
                          {
                              "success": true
                              "erased": "internalfs"
                          }
```
