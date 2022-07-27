# Libesphttpd WiFi-API

Functions to configure ESP32 WiFi settings via HTTP API.

## GUI
See the example js/html code for the GUI here: https://github.com/chmorgan/esphttpd-freertos/blob/master/html/wifi/index.html

## Functions defined in libesphttpd/cgiwifi.h

* __cgiWiFiScan()__

  Gets the results of an earler scan in JSON format.  Optionally start a new scan.
 
  Examples: 
   * `http://my-esp32-ip/wifi/scan?clear=1&start=1` - Clear the previous results and start a new scan. Returned APs list will be empty and inProgress will be true. 
      
      Note: If client is connected via WiFi, then start=1 may interrupt communication breifly, so use sparingly.
   * `http://my-esp32-ip/wifi/scan` - After sending start command, poll this until `inProgress:false` and APs list contains results.
     
     Note:  "enc" value is from `enum wifi_auth_mode_t`, where 0=Open, 1=WEP, 2+ is WPA.
  
  GET/POST args: 
  ```js
   "clear": number // 1: Clear the previous results first.
   "start": number // 1: Start a new scan now.
  ```
  Response:
  ```js
   {
      "args": {               // Args are repeated here in the response
          "clear": number,
          "start": number,
      },
      "APs": [{
          "essid": string,    // Name of AP discovered
          "bssid": string,    // MAC of AP discoverd
          "rssi": number,     // Signal strength i.e. -55
          "enc": number,      // WiFi security (encryption) type.
          "channel": number   // Channel used by AP
       },{...}],
      "working": boolean,     // A scan is in progress. Poll this.
      "success": boolean,     // CGI success/fail
      "error": string,        // Optional error message if failure
   }
  ```

* __cgiWiFiConnect()__

  Set WiFi STAtion (ESP WiFI Client) settings and trigger a connection.

  Note: The "success" response of this CGI call does not indicate if the WiFi connection succeeds. Poll /wifi/sta (cgiWiFiConnStatus) for connection pending/success/fail.  

  Examples: 
   * http://my-esp32-ip/wifi/connect?ssid=my-ssid&pass=mysecretpasswd - Trigger a connection attempt to the AP with the given SSID and password. 

  GET/POST args: 
  ```js
   "ssid": string
   "pass": string
  ```
  Response:
  ```js
   {
      "args": {               // Args are repeated here in the response
          "ssid": string,
          "pass": string,
      },
      "success": boolean,     // CGI success/fail
      "error": string,        // Optional error message if failure
   }
  ```

* __cgiWiFiSetMode()__

  CGI used to get/set the WiFi mode.  
  
  The mode values are defined by `enum wifi_mode_t`
  ```c
  0 /**< null mode */
  1 /**< WiFi station mode */
  2 /**< WiFi soft-AP mode */
  3 /**< WiFi station + soft-AP mode */
  ```

  Examples
  * i.e. http://ip/wifi/mode?mode=1  - Change mode to WIFI_MODE_STA

  GET/POST args: 
  ```js
   "mode": number // The desired Mode (as number specified in enum wifi_mode_t)
   "force": number // 1: Force the change, regardless of whether ESP's STA is connected to an AP.
  ```
  Response:
  ```js
   {
      "args": {               // Args are repeated here in the response
          "mode": number,
          "force": number,
      },
      "mode": number,         // The current Mode (as number specified in enum wifi_mode_t)
      "mode_str": string,     // The current Mode (as a string specified in wifi_mode_names[]= "Disabled","STA","AP""AP+STA")
      "success": boolean,     // CGI success/fail
      "error": string,        // Optional error message if failure
   }
  ```

* __cgiWiFiStartWps()__

  CGI for triggering a WPS push button connection attempt.

* __cgiWiFiAPSettings()__

  CGI for get/set settings in AP mode. 

  Examples: 
   * http://ip/wifi/ap?ssid=myssid&pass=mypass&chan=1  - Change AP settings

  GET/POST args: 
  ```js
   "chan": number,
   "ssid": string,
   "pass": string
  ```
  Response:
  ```js
   {
      "args": {               // Args are repeated here in the response
          "chan": number,
          "ssid": string,
          "pass": string,
      },
      "enabled" : boolean,    // AP is enabled
      "success": boolean,     // CGI success/fail
      "error": string,        // Optional error message if failure
   }
  ```

* __cgiWiFiConnStatus()__

  CGI returning the current state of the WiFi STA connection to an AP.

  Examples: 
   * `http://my-esp32-ip/wifi/sta` - Get the state of the STAtion
      
  Response:
  ```js
   {
      "ssid": string,         // SSID that the STAtion should connect to.
      "pass": string,         // WiFi network password.
      "enabled" : boolean,    // STA is enabled
      "ip" : string,          // Optional IP address of STAtion (only if connected)
      "working": boolean,     // A connect is in progress. Poll this.
      "connected": boolean,   // STAtion is connected to a WiFi network.  Poll this.
      "success": boolean,     // CGI success/fail
      "error": string,        // Optional error message if failure
   }
  ```
