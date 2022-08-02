/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
ESP32 Cgi/template routines for the /wifi url.
 */

#include <libesphttpd/cgiwifi.h>
#include <libesphttpd/esp.h>

#if defined(ESP32)
#include <errno.h>
#include <stdatomic.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/timers.h>

#include <esp_event.h>
#include <lwip/inet.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_wps.h>

/* Enable this to show verbose logging for this file only. */
//#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <esp_log.h>

#include "cJSON.h"
#include "libesphttpd/cgi_common.h"
#include <libesphttpd/kref.h>

static const char *TAG = "cgiwifi";
/* Enable this to disallow any changes in AP settings. */
//#define DEMO_MODE

#define MAX_NUM_APS 32
#define SCAN_TIMEOUT (60 * 1000 / portTICK_PERIOD_MS)
#define WPS_TIMEOUT (60 * 1000 / portTICK_PERIOD_MS)
#define CONNECT_TIMEOUT (10 * 1000 / portTICK_PERIOD_MS)
#define WATCHDOG_TIMEOUT (30 * 1000 / portTICK_PERIOD_MS)
#define CFG_TICKS (1000 / portTICK_PERIOD_MS)
#define CFG_DELAY (100 / portTICK_PERIOD_MS)

#define ARGBUFSIZE (16)

/* Jiffy overflow handling stolen from Linux kernel. Needed to check *\
\* for timeouts.                                                     */
#define typecheck(type, x)                 \
    (                                      \
        {                                  \
            type __dummy;                  \
            typeof(x) __dummy2;            \
            (void)(&__dummy == &__dummy2); \
            1;                             \
        })

#define time_after(a, b)           \
    (typecheck(unsigned int, a) && \
     typecheck(unsigned int, b) && \
     ((long)((b) - (a)) < 0))

/* States used during WiFi (re)configuration. */
enum cfg_state
{
    /* "stable" states */
    cfg_state_failed,
    cfg_state_connected,
    cfg_state_idle,

    /* transitional states */
    cfg_state_update,
    cfg_state_wps_start,
    cfg_state_wps_active,
    cfg_state_connecting,
    cfg_state_fallback,
};

const char *state_names[] = {
    "Failed",
    "Connected",
    "Idle",
    "Update",
    "WPS Start",
    "WPS Active",
    "Connecting",
    "Fall Back"};

// corresponding to wifi_mode_t
static const char *wifi_mode_names[] = {
    [WIFI_MODE_NULL] = "Disabled",
    [WIFI_MODE_STA] = "STA",      /**< WiFi station mode */
    [WIFI_MODE_AP] = "AP",        /**< WiFi soft-AP mode */
    [WIFI_MODE_APSTA] = "AP+STA", /**< WiFi station + soft-AP mode */
    [WIFI_MODE_MAX] = "invalid"};

/* Holds complete WiFi config for both STA and AP, the mode and whether *\
\* the WiFi should connect to an AP in STA or APSTA mode.               */
struct wifi_cfg
{
    bool connect;
    wifi_mode_t mode;
    wifi_config_t sta;
    wifi_config_t ap;
};

/* This holds all the information needed to transition from the current  *\
 * to the requested WiFi configuration. See handle_config_timer() and    *
\* update_wifi() on how to use this.                                     */
struct wifi_cfg_state
{
    SemaphoreHandle_t lock;
    TickType_t timestamp;
    enum cfg_state state;
    struct wifi_cfg saved;
    struct wifi_cfg new;
};

static struct wifi_cfg_state cfg_state;

/* For keeping track of system events. */
const static int BIT_CONNECTED = BIT0;
const static int BIT_WPS_SUCCESS = BIT1;
const static int BIT_WPS_FAILED = BIT2;
#define BITS_WPS (BIT_WPS_SUCCESS | BIT_WPS_FAILED)
const static int BIT_STA_STARTED = BIT3;

static EventGroupHandle_t wifi_events = NULL;

struct scan_data
{
    struct kref ref_cnt;
    wifi_ap_record_t *ap_records;
    uint16_t num_records;
};

static volatile atomic_bool scan_in_progress = ATOMIC_VAR_INIT(false);
static SemaphoreHandle_t data_lock = NULL;
static struct scan_data *last_scan = NULL;
static TimerHandle_t *scan_timer = NULL;
static TimerHandle_t *config_timer = NULL;

static void handle_scan_timer(TimerHandle_t timer);
static void handle_config_timer(TimerHandle_t timer);
static void cgiwifi_event_handler(void *, esp_event_base_t, int32_t, void *);

/* Initialise data structures. Needs to be called before any other function, *\
\* including the system event handler.                                       */
esp_err_t initCgiWifi(void)
{
    esp_err_t result;

    configASSERT(wifi_events == NULL);
    configASSERT(data_lock == NULL);
    configASSERT(cfg_state.lock == NULL);
    configASSERT(scan_timer == NULL);
    configASSERT(config_timer == NULL);

    result = ESP_OK;
    memset(&cfg_state, 0x0, sizeof(cfg_state));
    cfg_state.state = cfg_state_idle;

    wifi_events = xEventGroupCreate();
    if (wifi_events == NULL)
    {
        ESP_LOGE(TAG, "Unable to create event group.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }

    data_lock = xSemaphoreCreateMutex();
    if (data_lock == NULL)
    {
        ESP_LOGE(TAG, "Unable to create scan data lock.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }

    cfg_state.lock = xSemaphoreCreateMutex();
    if (cfg_state.lock == NULL)
    {
        ESP_LOGE(TAG, "Unable to create state lock.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }

    scan_timer = xTimerCreate("Scan_Timer",
                              SCAN_TIMEOUT,
                              pdFALSE, NULL, handle_scan_timer);
    if (scan_timer == NULL)
    {
        ESP_LOGE(TAG, "[%s] Failed to create scan timeout timer",
                 __FUNCTION__);
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }

    config_timer = xTimerCreate("Config_Timer",
                                CFG_TICKS,
                                pdFALSE, NULL, handle_config_timer);
    if (config_timer == NULL)
    {
        ESP_LOGE(TAG, "[%s] Failed to create config validation timer",
                 __FUNCTION__);
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &cgiwifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &cgiwifi_event_handler,
                                                        NULL,
                                                        NULL));
err_out:
    if (result != ESP_OK)
    {
        if (wifi_events != NULL)
        {
            vEventGroupDelete(wifi_events);
            wifi_events = NULL;
        }

        if (data_lock != NULL)
        {
            vSemaphoreDelete(data_lock);
            data_lock = NULL;
        }

        if (cfg_state.lock != NULL)
        {
            vSemaphoreDelete(cfg_state.lock);
            cfg_state.lock = NULL;
        }

        if (scan_timer != NULL)
        {
            xTimerDelete(scan_timer, 0);
            scan_timer = NULL;
        }

        if (config_timer != NULL)
        {
            xTimerDelete(config_timer, 0);
            config_timer = NULL;
        }
    }

    return result;
}

/* Get a reference counted pointer to the current set of AP scan data. *\
\* Must be released via put_scan_data().                               */
static struct scan_data *get_scan_data(void)
{
    struct scan_data *data = NULL;
    configASSERT(data_lock != NULL);
    if (xSemaphoreTake(data_lock, CFG_DELAY) == pdTRUE)
    {
        if (last_scan != NULL)
        {
            data = last_scan;
            kref_get(&(data->ref_cnt));
        }
        xSemaphoreGive(data_lock);
    }
    return data;
}

/* Free scan data, should only be called kref_put(). */
static void free_scan_data(struct kref *ref)
{
    struct scan_data *data;

    data = kcontainer_of(ref, struct scan_data, ref_cnt);
    free(data->ap_records);
    free(data);
    last_scan = NULL;
}

/* Drop a reference to a scan data set, possibly freeing it. */
static void put_scan_data(struct scan_data *data)
{
    configASSERT(data != NULL);

    kref_put(&(data->ref_cnt), free_scan_data);
}

/* Clear the AP scan data by Drop the global reference to the scan data set, eventually freeing it. */
static void clear_global_scan_data(void)
{
    configASSERT(data_lock != NULL);
    if (xSemaphoreTake(data_lock, CFG_DELAY) == pdTRUE)
    {
        if (last_scan != NULL)
        {
            /* Drop global reference to old data set so it will be freed    *\
            \* when the last connection using it gets closed.               */
            put_scan_data(last_scan);
        }
        xSemaphoreGive(data_lock);
    }
}

/* Fetch the latest AP scan data and make it available. */
static void wifi_scan_done(wifi_event_sta_scan_done_t *event)
{
    uint16_t num_aps;
    struct scan_data *old, *new;
    esp_err_t result;

    result = ESP_OK;
    new = NULL;

    /* cgiWifiSetup() must have been called prior to this point. */
    configASSERT(data_lock != NULL);

    if (atomic_load(&scan_in_progress) == false)
    {
        /* Either scan was cancelled due to timeout or somebody else *\
        \* is triggering scans.                                      */
        ESP_LOGE(TAG, "[%s] Received unsolicited scan done event.",
                 __FUNCTION__);
        return;
    }

    if (event->status != ESP_OK)
    {
        ESP_LOGI(TAG, "Scan failed. Event status: 0x%x",
                 event->status);
        goto err_out;
    }

    /* Fetch number of APs found. Bail out early if there is nothing to get. */
    result = esp_wifi_scan_get_ap_num(&num_aps);
    if (result != ESP_OK || num_aps == 0)
    {
        ESP_LOGI(TAG, "Scan error or empty scan result");
        goto err_out;
    }

    /* Limit number of records to fetch. Prevents possible DoS by tricking   *\
    \* us into allocating storage for a very large amount of scan results.   */
    if (num_aps > MAX_NUM_APS)
    {
        ESP_LOGI(TAG, "Limiting AP records to %d (Actually found %d)",
                 MAX_NUM_APS, num_aps);
        num_aps = MAX_NUM_APS;
    }

    /* Allocate and initialise memory for scan data and AP records. */
    new = calloc(1, sizeof(*new));
    if (new == NULL)
    {
        ESP_LOGE(TAG, "Out of memory creating scan data");
        goto err_out;
    }

    kref_init(&(new->ref_cnt)); // initialises ref_cnt to 1
    new->ap_records = calloc(num_aps, sizeof(*(new->ap_records)));
    if (new->ap_records == NULL)
    {
        ESP_LOGE(TAG, "Out of memory for fetching records");
        goto err_out;
    }

    /* Fetch actual AP scan data */
    new->num_records = num_aps;
    result = esp_wifi_scan_get_ap_records(&(new->num_records), new->ap_records);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Error getting scan results");
        goto err_out;
    }

    ESP_LOGI(TAG, "Scan done: found %d APs", num_aps);

    /* Make new scan data available. */
    if (xSemaphoreTake(data_lock, portTICK_PERIOD_MS) == pdTRUE)
    {
        /* The new data set will be asigned to the global pointer, so fetch *\
        \* another reference.                                               */
        kref_get(&(new->ref_cnt));

        old = last_scan;
        last_scan = new;

        if (old != NULL)
        {
            /* Drop global reference to old data set so it will be freed    *\
            \* when the last connection using it gets closed.               */
            put_scan_data(old);
        }

        xSemaphoreGive(data_lock);
    }

err_out:
    /* Drop one reference to the new scan data. */
    if (new != NULL)
    {
        put_scan_data(new);
    }

    /* Clear scan flag so a new scan can be triggered. */
    atomic_store(&scan_in_progress, false);
    if (scan_timer != NULL)
    {
        xTimerStop(scan_timer, 0);
    }
}

/* Timer function to stop a hanging AP scan. */
static void handle_scan_timer(TimerHandle_t timer)
{
    atomic_bool tmp = ATOMIC_VAR_INIT(true);

    if (atomic_compare_exchange_strong(&scan_in_progress, &tmp, true) == true)
    {
        ESP_LOGI(TAG, "[%s] Timeout, stopping scan.", __FUNCTION__);
        (void)esp_wifi_scan_stop();
        atomic_store(&scan_in_progress, false);
    }
}

/* Function to trigger an AP scan. */
static esp_err_t wifi_start_scan(void)
{
    wifi_scan_config_t scan_cfg;
    wifi_mode_t mode;
    esp_err_t result;

    /* Make sure we do not try to start a scan while the WiFi config is *\
    \* is in a transitional state.                                      */
    if (xSemaphoreTake(cfg_state.lock, CFG_DELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "[%s] Unable to acquire config lock.", __FUNCTION__);
        return ESP_FAIL;
    }

    if (cfg_state.state > cfg_state_idle)
    {
        ESP_LOGI(TAG, "[%s] WiFi connecting, not starting scan.", __FUNCTION__);
        result = ESP_FAIL;
        goto err_out;
    }

    /* Check that we are in a suitable mode for scanning. */
    result = esp_wifi_get_mode(&mode);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] Error fetching WiFi mode.", __FUNCTION__);
        goto err_out;
    }

    if (mode != WIFI_MODE_APSTA && mode != WIFI_MODE_STA)
    {
        ESP_LOGE(TAG, "[%s] Invalid WiFi mode for scanning.", __FUNCTION__);
        result = ESP_FAIL;
        goto err_out;
    }

    /* Finally, start a scan. Unless there is one running already. */
    if (atomic_exchange(&scan_in_progress, true) == false)
    {
        ESP_LOGI(TAG, "Starting scan.");

        memset(&scan_cfg, 0x0, sizeof(scan_cfg));
        scan_cfg.show_hidden = true;
        scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;

        result = esp_wifi_scan_start(&scan_cfg, false);
        if (result == ESP_OK)
        {
            ESP_LOGD(TAG, "[%s] Starting timer.", __FUNCTION__);

            /* Trigger the timer so scan will be aborted after timeout. */
            xTimerReset(scan_timer, 0);
        }
        else
        {
            ESP_LOGE(TAG, "[%s] Starting AP scan failed.", __FUNCTION__);

            atomic_store(&scan_in_progress, false);
        }
    }
    else
    {
        ESP_LOGD(TAG, "[%s] Scan already running.", __FUNCTION__);
        result = ESP_OK;
    }

err_out:
    xSemaphoreGive(cfg_state.lock);
    return result;
}

/* Get the results of an earler scan in JSON format. . Optionally start a new scan.
 * See API spec: /README-wifi_api.md
 */
CgiStatus cgiWiFiScan(HttpdConnData *connData)
{
    cJSON *jsroot = NULL;

    if (connData->isConnectionClosed)
    {
        goto cleanup; // make sure to free memory
    }

    if (connData->requestType != HTTPD_METHOD_GET && connData->requestType != HTTPD_METHOD_POST)
    {
        return HTTPD_CGI_NOTFOUND;
    }

    /* First call. */
    if (connData->cgiData == NULL) // persistent data stored on connData->cgiData
    {
        // First call to this cgi.
        bool success = false;
        jsroot = cJSON_CreateObject();
        wifi_mode_t mode;

        char *allArgs = connData->getArgs;
        // Make it work with either GET or POST
        if (connData->requestType == HTTPD_METHOD_POST)
        {
            allArgs = connData->post.buff;
        }
        cJSON *jsargs = cJSON_AddObjectToObject(jsroot, "args");
        char arg_buf[ARGBUFSIZE];

        uint32_t arg_clear = 0;
        if (cgiGetArgDecU32(allArgs, "clear", &(arg_clear), arg_buf, sizeof(arg_buf)))
        {
            cJSON_AddNumberToObject(jsargs, "clear", arg_clear);
        }

        uint32_t arg_start = 0;
        if (cgiGetArgDecU32(allArgs, "start", &(arg_start), arg_buf, sizeof(arg_buf)))
        {
            cJSON_AddNumberToObject(jsargs, "start", arg_start);
        }

        if (esp_wifi_get_mode(&mode) != ESP_OK)
        {
            const char *err_str = "Error fetching WiFi mode.";
            ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "error", err_str);
            goto err_out;
        }

        if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) /* Skip sending stale AP data if we are in AP mode. */
        {
            const char *err_str = "Invalid WiFi mode for scanning.";
            ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "error", err_str);
            goto err_out;
        }

        if (arg_clear != 0)
        {
            clear_global_scan_data();
        }
        if (arg_start != 0)
        {
            if (wifi_start_scan() != ESP_OK) // start a new scan
            {
                /* Start_scan failed. Tell the user there is an error and don't just keep trying.  */
                const char *err_str = "Start scan failed.";
                ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
                cJSON_AddStringToObject(jsroot, "error", err_str);
                goto err_out;
            }
        }

        struct scan_data *scanData = get_scan_data();
        if (scanData == NULL)
        {
            // No scan data available yet. Tell the user we are still trying...
            success = true;
        }
        else
        {
            // We have data to send.
            cJSON *jsAps = cJSON_AddArrayToObject(jsroot, "APs");
            for (int idx = 0; idx < scanData->num_records; idx++)
            {
                wifi_ap_record_t *record = &(scanData->ap_records[idx]);
                cJSON *jsAp = cJSON_CreateObject();
                char buff[32];
                cJSON_AddStringToObject(jsAp, "essid", (char *)record->ssid);
                snprintf((char *)buff, sizeof(buff), MACSTR, MAC2STR(record->bssid)); // convert MAC to string
                cJSON_AddStringToObject(jsAp, "bssid", buff);
                cJSON_AddNumberToObject(jsAp, "rssi", record->rssi);
                cJSON_AddNumberToObject(jsAp, "enc", record->authmode);
                cJSON_AddNumberToObject(jsAp, "channel", record->primary);
                cJSON_AddItemToArray(jsAps, jsAp);
            }
            success = true;
            put_scan_data(scanData); // drop reference to scan data
        }
        //// All done.  Send the results
    err_out:
        cJSON_AddBoolToObject(jsroot, "working", atomic_load(&scan_in_progress));
        cJSON_AddBoolToObject(jsroot, "success", success);
        cgiJsonResponseHeaders(connData);
    }

cleanup:
    return cgiJsonResponseCommonMulti(connData, &connData->cgiData, jsroot); // Send the json response!
}

/* Helper function to check if WiFi is connected in station mode. */
static bool sta_connected(void)
{
    EventBits_t events;

    events = xEventGroupGetBits(wifi_events);

    return !!(events & BIT_CONNECTED);
}

/* Helper function to set WiFi configuration from struct wifi_cfg. */
static void set_wifi_cfg(struct wifi_cfg *cfg)
{
    esp_err_t result;

    if (cfg->mode == WIFI_MODE_NULL)
    {
        // turning off WiFi
        result = esp_wifi_stop();
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] esp_wifi_stop(): %d %s",
                     __FUNCTION__, result, esp_err_to_name(result));
        }
    }

    /* FIXME: we should check for errors. OTOH, this is also used  *\
     *        for the fall-back mechanism, so aborting on error is *
    \*        probably a bad idea.                                 */
    result = esp_wifi_set_mode(cfg->mode);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] esp_wifi_set_mode(): %d %s",
                 __FUNCTION__, result, esp_err_to_name(result));
    }

    if (cfg->mode == WIFI_MODE_NULL)
    {
        // turning off WiFi
        return;
    }

    if (cfg->mode == WIFI_MODE_APSTA || cfg->mode == WIFI_MODE_AP)
    {
        result = esp_wifi_set_config(WIFI_IF_AP, &(cfg->ap));
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] esp_wifi_set_config() AP: %d %s",
                     __FUNCTION__, result, esp_err_to_name(result));
        }
    }

    if (cfg->mode == WIFI_MODE_APSTA || cfg->mode == WIFI_MODE_STA)
    {
        result = esp_wifi_set_config(WIFI_IF_STA, &(cfg->sta));
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] esp_wifi_set_config() STA: %d %s",
                     __FUNCTION__, result, esp_err_to_name(result));
        }
    }

    result = esp_wifi_start();
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] esp_wifi_start(): %d %s",
                 __FUNCTION__, result, esp_err_to_name(result));
    }

    if (cfg->connect && (cfg->mode == WIFI_MODE_STA || cfg->mode == WIFI_MODE_APSTA))
    {
        result = esp_wifi_connect();
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] esp_wifi_connect(): %d %s",
                     __FUNCTION__, result, esp_err_to_name(result));
        }
    }
}

/* Helper to store current WiFi configuration into a struct wifi_cfg. */
static esp_err_t get_wifi_cfg(struct wifi_cfg *cfg)
{
    esp_err_t result;

    result = ESP_OK;
    memset(cfg, 0x0, sizeof(*cfg));

    cfg->connect = sta_connected();

    result = esp_wifi_get_config(WIFI_IF_STA, &(cfg->sta));
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] Error fetching STA config.", __FUNCTION__);
        goto err_out;
    }

    result = esp_wifi_get_config(WIFI_IF_AP, &(cfg->ap));
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] Error fetching AP config.", __FUNCTION__);
        goto err_out;
    }

    result = esp_wifi_get_mode(&(cfg->mode));
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] Error fetching WiFi mode.", __FUNCTION__);
        goto err_out;
    }

err_out:
    return result;
}

/* This function is called from the config_timer and handles all WiFi        *\
 * configuration changes. It takes its information from the global           *
 * cfg_state struct and tries to set the WiFi configuration to the one       *
 * found in the "new" member. If things go wrong, it will try to fall        *
 * back to the configuration found in "saved". This should minimise          *
 * the risk of users locking themselves out of the device by setting         *
 * wrong WiFi credentials in STA-only mode.                                  *
 *                                                                           *
 * This function will keep triggering itself until it reaches a "stable"     *
 * (idle, connected, failed) state in cfg_state.state.                       *
 *                                                                           *
 * cfg_state must not be modified without first obtaining the cfg_state.lock *
 * mutex and then checking that cfg_state.state is in a stable state.        *
 * To set a new configuration, just store the current config to .saved,      *
 * update .new to the desired config, set .state to cfg_state_update         *
 * and start the config_timer.                                               *
 * To connect to an AP with WPS, save the current state, set .state          *
 * to cfg_state_wps_start and start the config_timer.                        *
 \*                                                                          */
static void handle_config_timer(TimerHandle_t timer)
{
    bool connected;
    wifi_mode_t mode;
    esp_wps_config_t config = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
    TickType_t now, delay;
    EventBits_t events;
    esp_err_t result;

    /* If we can not get the config state lock, we try to reschedule the    *\
     * timer. If that also fails, we are SOL...                             *
    \* Maybe we should trigger a reboot.                                    */
    if (xSemaphoreTake(cfg_state.lock, 0) != pdTRUE)
    {
        if (xTimerChangePeriod(config_timer, CFG_DELAY, CFG_DELAY) != pdPASS)
        {
            ESP_LOGE(TAG, "[%s] Failure to get config lock and change timer.",
                     __FUNCTION__);
        }
        return;
    }

    ESP_LOGD(TAG, "[%s] Called. State: %s",
             __FUNCTION__, state_names[cfg_state.state]);

    /* If delay gets set later, the timer will be re-scheduled on exit. */
    delay = 0;

    /* Gather various information about the current system state. */
    connected = sta_connected();
    events = xEventGroupGetBits(wifi_events);
    now = xTaskGetTickCount();

    result = esp_wifi_get_mode(&mode);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] Error fetching WiFi mode.", __FUNCTION__);
        cfg_state.state = cfg_state_failed;
        goto err_out;
    }

    switch (cfg_state.state)
    {
    case cfg_state_wps_start:

        /* Try connecting to AP with WPS. First, tear down any connection *\
        \* we might currently have.                                       */
        get_wifi_cfg(&cfg_state.new);
        memset(&cfg_state.new.sta, 0x0, sizeof(cfg_state.new.sta));
        cfg_state.new.mode = WIFI_MODE_APSTA;
        cfg_state.new.connect = false;

        set_wifi_cfg(&cfg_state.new);

        /* Clear previous results and start WPS. */
        xEventGroupClearBits(wifi_events, BITS_WPS);
        result = esp_wifi_wps_enable(&config);
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] esp_wifi_wps_enable() failed: %d %s",
                     __FUNCTION__, result, esp_err_to_name(result));
            cfg_state.state = cfg_state_fallback;
            delay = CFG_DELAY;
        }

        result = esp_wifi_wps_start(0);
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] esp_wifi_wps_start() failed: %d %s",
                     __FUNCTION__, result, esp_err_to_name(result));
            cfg_state.state = cfg_state_fallback;
            delay = CFG_DELAY;
        }

        /* WPS is running, set time stamp and transition to next state. */
        cfg_state.timestamp = now;
        cfg_state.state = cfg_state_wps_active;
        delay = CFG_TICKS;
        break;
    case cfg_state_wps_active:
        /* WPS is running. Check for events and timeout. */
        if (events & BIT_WPS_SUCCESS)
        {
            /* WPS succeeded. Disable WPS and use the received credentials *\
             * to connect to the AP by transitioning to the updating state.*/
            ESP_LOGI(TAG, "[%s] WPS success.", __FUNCTION__);
            result = esp_wifi_wps_disable();
            if (result != ESP_OK)
            {
                ESP_LOGE(TAG, "[%s] wifi wps disable: %d %s",
                         __FUNCTION__, result, esp_err_to_name(result));
            }

            /* Get received STA config, then force APSTA mode, set  *\
            \* connect flag and trigger update.                     */
            get_wifi_cfg(&cfg_state.new);
            cfg_state.new.mode = WIFI_MODE_APSTA;
            cfg_state.new.connect = true;
            cfg_state.state = cfg_state_update;
            delay = CFG_DELAY;
        }
        else if (time_after(now, (cfg_state.timestamp + WPS_TIMEOUT)) || (events & BIT_WPS_FAILED))
        {
            /* Failure or timeout. Trigger fall-back to the previous config. */
            ESP_LOGI(TAG, "[%s] WPS failed, restoring saved config.",
                     __FUNCTION__);

            result = esp_wifi_wps_disable();
            if (result != ESP_OK)
            {
                ESP_LOGE(TAG, "[%s] wifi wps disable: %d %s",
                         __FUNCTION__, result, esp_err_to_name(result));
            }

            cfg_state.state = cfg_state_fallback;
            delay = CFG_DELAY;
        }
        else
        {
            delay = CFG_TICKS;
        }
        break;
    case cfg_state_update:
        /* Start changing WiFi to new configuration. */
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_disconnect();
        set_wifi_cfg(&(cfg_state.new));

        if (cfg_state.new.mode == WIFI_MODE_AP ||
            cfg_state.new.mode == WIFI_MODE_NULL ||
            !cfg_state.new.connect)
        {
            /* AP-only mode or not connecting, we are done. */
            cfg_state.state = cfg_state_idle;
        }
        else
        {
            /* System should now connect to the AP. */
            cfg_state.timestamp = now;
            cfg_state.state = cfg_state_connecting;
            delay = CFG_TICKS;
        }
        break;
    case cfg_state_connecting:
        /* We are waiting for a connection to an AP. */
        if (connected)
        {
            /* We have a connection! \o/ */
            cfg_state.state = cfg_state_connected;
        }
        else if (time_after(now, (cfg_state.timestamp + CONNECT_TIMEOUT)))
        {
            /* Timeout while waiting for connection. Try falling back to the *\
            \* saved configuration.                                          */
            cfg_state.state = cfg_state_fallback;
            delay = CFG_DELAY;
        }
        else
        {
            /* Twiddle our thumbs and keep waiting for the connection.  */
            delay = CFG_TICKS;
        }
        break;
    case cfg_state_fallback:
        /* Something went wrong, try going back to the previous config. */
        ESP_LOGI(TAG, "[%s] restoring saved Wifi config.", __FUNCTION__);
        ESP_LOGD(TAG, "Saved Mode:%s, Connect:%d", wifi_mode_names[cfg_state.saved.mode], cfg_state.saved.connect);
        (void)esp_wifi_disconnect();
        memmove(&cfg_state.new, &cfg_state.saved, sizeof(cfg_state.new)); // copy saved over new
        set_wifi_cfg(&(cfg_state.new));
        cfg_state.state = cfg_state_failed;
        break;

    case cfg_state_connected:
        // Sync up the state.  This cgiWifi module should work alongside other WiFi management i.e. BlueFi.
        get_wifi_cfg(&cfg_state.new);
        cfg_state.new.connect = true; // because we are in the connected state.
        break;
    case cfg_state_idle:
    case cfg_state_failed:
        /**
         * Watchdog to make sure the STA stays connected.
         * This is useful since ESP-IDF has trouble reconnecting STA sometimes (i.e. you reboot your router).
         * Makes sure that the current WiFi state is consistent with "cfg_state.new".
         * This does not connect STA on Boot-up if WiFi was previously configured, use startCgiWifi() for that.
         */
        if (cfg_state.new.mode == WIFI_MODE_AP ||
            cfg_state.new.mode == WIFI_MODE_NULL ||
            !cfg_state.new.connect)
        {
            /* AP-only mode or not supposed to be connected, so nothing to check. */
            ESP_LOGD(TAG, "Wifi config watchdog skipped b/c Mode:%s, Connect:%d", wifi_mode_names[cfg_state.new.mode], cfg_state.new.connect);
        }
        else
        {
            /* System should be connected to the AP, make sure it is. */
            if (sta_connected())
            {
                ESP_LOGD(TAG, "Wifi config watchdog OK.");
            }
            else
            {
                ESP_LOGI(TAG, "Wifi config watchdog triggered!  Retry connect to STA.");
                (void)esp_wifi_disconnect();
                set_wifi_cfg(&(cfg_state.new));
            }
        }
        break;
    }

err_out:
    if (delay == 0)
    {
        delay = WATCHDOG_TIMEOUT; // check watch-dog-timer
    }
    /* We are in a transitional state, re-arm the timer. */
    if (xTimerChangePeriod(config_timer, delay, CFG_DELAY) != pdPASS)
    {
        cfg_state.state = cfg_state_failed;
    }

    ESP_LOGD(TAG, "[%s] Leaving. State: %s delay: %d",
             __FUNCTION__, state_names[cfg_state.state], delay);

    xSemaphoreGive(cfg_state.lock);
    return;
}

/* Update state information from system events.    */
static void cgiwifi_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        xEventGroupSetBits(wifi_events, BIT_STA_STARTED);
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP)
    {
        xEventGroupClearBits(wifi_events, BIT_STA_STARTED);
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE)
    {
        wifi_scan_done((wifi_event_sta_scan_done_t *)event_data);
    }
    if ((event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) ||
        (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6))
    {
        xEventGroupSetBits(wifi_events, BIT_CONNECTED);
    }
    if ((event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) ||
        (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED))
    {
        xEventGroupClearBits(wifi_events, BIT_CONNECTED);
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_WPS_ER_SUCCESS)
    {
        xEventGroupSetBits(wifi_events, BIT_WPS_SUCCESS);
    }
    if ((event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_WPS_ER_FAILED) ||
        (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_WPS_ER_PIN) ||
        (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_WPS_ER_TIMEOUT))
    {
        xEventGroupSetBits(wifi_events, BIT_WPS_FAILED);
    }
}

/* Set a new WiFi configuration. This function will save the current config *\
 * to cfg->saved, then compare it to the requested new configuration. If    *
 * the two configurations are different, it will store the new config in    *
\* cfg->new and trigger the asynchronous mechanism to handle the update.    */
static esp_err_t update_wifi(struct wifi_cfg_state *cfg, struct wifi_cfg *new, bool no_fallback)
{
    bool connected;
    bool update;
    esp_err_t result;

    if (xSemaphoreTake(cfg->lock, CFG_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "[%s] Error taking mutex.", __FUNCTION__);
        return ESP_ERR_TIMEOUT;
    }

    if (new->mode != WIFI_MODE_NULL && cfg->state > cfg_state_idle)
    {
        ESP_LOGI(TAG, "[%s] Already connecting.", __FUNCTION__);
        result = ESP_ERR_INVALID_STATE;
        goto err_out;
    }

    result = ESP_OK;
    update = false;
    memmove(&(cfg->new), new, sizeof(cfg->new));

    if (no_fallback)
    {
        memmove(&(cfg->saved), new, sizeof(cfg->saved)); // Also copy new to saved if no fallback.
        update = true;                                   // force the update
    }
    else
    {
        /* Save current configuration for fall-back. */
        result = get_wifi_cfg(&(cfg->saved));
        if (result != ESP_OK)
        {
            ESP_LOGI(TAG, "[%s] Error fetching current WiFi config.",
                     __FUNCTION__);
            goto err_out;
        }

        /* Clear station configuration if we are not connected to an AP. */
        connected = sta_connected();
        if (!connected)
        {
            memset(&(cfg->saved.sta), 0x0, sizeof(cfg->saved.sta));
        }

        /* Do some naive checks to see if the new configuration is an actual   *\
        \* change. Should be more thorough by actually comparing the elements. */
        if (cfg->new.mode != cfg->saved.mode)
        {
            update = true;
        }

        if ((new->mode == WIFI_MODE_AP || new->mode == WIFI_MODE_APSTA) && memcmp(&(cfg->new.ap), &(cfg->saved.ap), sizeof(cfg->new.ap)))
        {
            update = true;
        }

        if ((new->mode == WIFI_MODE_STA || new->mode == WIFI_MODE_APSTA) && memcmp(&(cfg->new.sta), &(cfg->saved.sta), sizeof(cfg->new.sta)))
        {
            update = true;
        }
    }

    /* If new config is different, trigger asynchronous update. This gives *\
     * the httpd some time to send out the reply before possibly tearing   *
    \* down the connection.                                                */
    if (update == true)
    {
        cfg->state = cfg_state_update;
        if (xTimerChangePeriod(config_timer, CFG_DELAY, CFG_DELAY) != pdPASS)
        {
            cfg->state = cfg_state_failed;
            result = ESP_ERR_TIMEOUT;
            goto err_out;
        }
    }
    else if (connected)
    {
        cfg->state = cfg_state_connected; // clear previous error
    }

err_out:
    xSemaphoreGive(cfg->lock);
    return result;
}

/**
 * (Optional) Start WiFi STA using saved settings.
 *  Call it from main task after calls to esp_event_loop_init() and esp_wifi_start().  (This may block until STA is started.)
 */
esp_err_t startCgiWifi(void)
{
    struct wifi_cfg cfg;
    wifi_sta_config_t *sta;
    esp_err_t result;
    EventBits_t uxBits;
    const TickType_t xTicksToWait = 100 / portTICK_PERIOD_MS;

    memset(&cfg, 0x0, sizeof(cfg));

    result = get_wifi_cfg(&cfg);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "[%s] Error fetching WiFi config.", __FUNCTION__);
        goto err_out;
    }
    if (cfg.mode != WIFI_MODE_STA && cfg.mode != WIFI_MODE_APSTA)
    {
        ESP_LOGI(TAG, "Startup WiFi STA disabled.");
        goto err_out;
    }
    sta = &(cfg.sta.sta);

    /* We want STA to actually connect to the AP. */
    cfg.connect = true;

    ESP_LOGI(TAG, "Startup connect to AP %s pw %s",
             sta->ssid, sta->password);

    uxBits = xEventGroupWaitBits(
        wifi_events,     /* The event group being tested. */
        BIT_STA_STARTED, /* The bits within the event group to wait for. */
        pdFALSE,         /* bits should not be cleared before returning. */
        pdFALSE,         /* Don't wait for both bits, either bit will do. */
        xTicksToWait);   /* Wait a maximum of 100ms for either bit to be set. */

    if (uxBits & BIT_STA_STARTED)
    {
        /* xEventGroupWaitBits() returned because BIT_STA_STARTED was set. */
        result = update_wifi(&cfg_state, &cfg, true);
    }
    else
    {
        /* xEventGroupWaitBits() returned because xTicksToWait ticks passed
      without BIT_STA_STARTED becoming set. */
        ESP_LOGE(TAG, "[%s] Error STA not started.", __FUNCTION__);
        result = ESP_FAIL;
        goto err_out;
    }

err_out:
    return result;
}

/* Trigger a connection attempt to the AP with the given SSID and password. */
/* Connect WiFi STA.  Use other API /wifi/sta to check status.
 * See API spec: /README-wifi_api.md
 */
CgiStatus cgiWiFiConnect(HttpdConnData *connData)
{
    if (connData->isConnectionClosed)
    {
        /* Connection aborted. Clean up. */
        return HTTPD_CGI_DONE;
    }

    if (connData->requestType != HTTPD_METHOD_GET && connData->requestType != HTTPD_METHOD_POST)
    {
        return HTTPD_CGI_NOTFOUND;
    }

    // Pointer to the args, could be either from GET or POST
    char *allArgs = connData->getArgs;
    if (connData->requestType == HTTPD_METHOD_POST)
    {
        allArgs = connData->post.buff;
    }

    int len;
    char arg_buf[ARGBUFSIZE];
    struct wifi_cfg cfg;
    esp_err_t result;
    cJSON *jsroot = cJSON_CreateObject();

    memset(&cfg, 0x0, sizeof(cfg));

    /* We are only changing SSID and password, so fetch the current *\
    \* configuration and update just these two entries.             */
    result = get_wifi_cfg(&cfg);
    if (result != ESP_OK)
    {
        const char *err_str = "Error fetching WiFi config.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        goto err_out;
    }

    wifi_sta_config_t *sta = &(cfg.sta.sta);
    cJSON *jsargs = cJSON_AddObjectToObject(jsroot, "args");
    len = httpdFindArg(allArgs, "ssid", arg_buf, sizeof(arg_buf));
    if (len > 0)
    {
        strlcpy((char *)&(sta->ssid), arg_buf, sizeof(sta->ssid));
        cJSON_AddStringToObject(jsargs, "ssid", (char *)sta->ssid);
    }

    len = httpdFindArg(allArgs, "pass", arg_buf, sizeof(arg_buf));
    if (len > 0)
    {
        strlcpy((char *)&(sta->password), arg_buf, sizeof(sta->password));
        cJSON_AddStringToObject(jsargs, "pass", (char *)sta->password);
    }
    else
    {
        sta->password[0] = 0; // empty password
    }

    /* And of course we want to actually connect to the AP. */
    cfg.connect = true;

#ifndef DEMO_MODE
    ESP_LOGI(TAG, "Trying to connect to AP %s pw %s",
             sta->ssid, sta->password);

    result = update_wifi(&cfg_state, &cfg, false);
    if (result != ESP_OK)
    {
        const char *err_str = "Setting WiFi config failed.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        goto err_out;
    }
#else
    {
        const char *err_str = "Demo mode, not actually connecting to AP.";
        ESP_LOGW(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "demo", err_str);
        result = ESP_OK; // fake OK status for demo
    }
#endif

err_out:
    cJSON_AddBoolToObject(jsroot, "success", (result == ESP_OK));
    return cgiJsonResponseCommonSingle(connData, jsroot); // Send the json response!
}

/* CGI used to get/set the WiFi mode.  See enum wifi_mode_t for the values.
 * See API spec: /README-wifi_api.md
 */
CgiStatus cgiWiFiSetMode(HttpdConnData *connData)
{
    if (connData->isConnectionClosed)
    {
        /* Connection aborted. Clean up. */
        return HTTPD_CGI_DONE;
    }

    if (connData->requestType != HTTPD_METHOD_GET && connData->requestType != HTTPD_METHOD_POST)
    {
        return HTTPD_CGI_NOTFOUND;
    }

    // Pointer to the args, could be either from GET or POST
    char *allArgs = connData->getArgs;
    if (connData->requestType == HTTPD_METHOD_POST)
    {
        allArgs = connData->post.buff;
    }

    struct wifi_cfg cfg;
    esp_err_t result = ESP_OK;
    cJSON *jsroot = cJSON_CreateObject();

    // Get current mode.
    memset(&cfg, 0x0, sizeof(cfg));
    result = get_wifi_cfg(&cfg);
    if (result != ESP_OK)
    {
        const char *err_str = "Error fetching current WiFi config.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        goto err_out;
    }

    char arg_buf[ARGBUFSIZE];
    cJSON *jsargs = cJSON_AddObjectToObject(jsroot, "args");

    uint32_t arg_force = 0;
    if (cgiGetArgDecU32(allArgs, "force", &(arg_force), arg_buf, sizeof(arg_buf)))
    {
        cJSON_AddNumberToObject(jsargs, "force", arg_force);
    }
    // Setting a new mode?
    wifi_mode_t new_mode;
    if (cgiGetArgDecU32(allArgs, "mode", &new_mode, arg_buf, sizeof(arg_buf)))
    {
        cJSON_AddNumberToObject(jsargs, "mode", new_mode);
        if (new_mode < WIFI_MODE_NULL || new_mode >= WIFI_MODE_MAX)
        {
            const char *err_str = "Invalid WiFi mode.";
            ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "error", err_str);
            result = -1;
            goto err_out;
        }

        /* Do not switch to STA mode without being connected to an AP. (Unless &force=1) */
        if (new_mode == WIFI_MODE_STA && cfg.mode == WIFI_MODE_APSTA && !sta_connected() && arg_force == 0)
        {
            const char *err_str = "No connection to AP, not switching to client-only mode.";
            ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "error", err_str);
            result = -1;
            goto err_out;
        }

        cfg.mode = new_mode;

#ifndef DEMO_MODE
        ESP_LOGI(TAG, "[%s] Switching to WiFi mode %s", __FUNCTION__, wifi_mode_names[new_mode]);

        result = update_wifi(&cfg_state, &cfg, false);
        if (result != ESP_OK)
        {
            const char *err_str = "Setting WiFi config failed.";
            ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "error", err_str);
            goto err_out;
        }
#else
        {
            const char *err_str = "Demo mode, not switching WiFi mode.";
            ESP_LOGW(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "demo", err_str);
            result = ESP_OK; // fake OK status for demo
        }
#endif
    }
    // else no mode argument, just return the current mode

    cJSON_AddNumberToObject(jsroot, "mode", cfg.mode);
    cJSON_AddStringToObject(jsroot, "mode_str", wifi_mode_names[cfg.mode]);

err_out:
    // changing mode takes some time, so indicate user to wait
    cJSON_AddBoolToObject(jsroot, "success", (result == ESP_OK));
    return cgiJsonResponseCommonSingle(connData, jsroot); // Send the json response!
}

/* CGI for triggering a WPS push button connection attempt. */
CgiStatus cgiWiFiStartWps(HttpdConnData *connData)
{
    if (connData->isConnectionClosed)
    {
        /* Connection aborted. Clean up. */
        return HTTPD_CGI_DONE;
    }

    if (connData->requestType != HTTPD_METHOD_GET && connData->requestType != HTTPD_METHOD_POST)
    {
        return HTTPD_CGI_NOTFOUND;
    }

    struct wifi_cfg cfg;
    esp_err_t result = ESP_OK;
    cJSON *jsroot = cJSON_CreateObject();

    /* Make sure we are not in the middle of setting a new WiFi config. */
    if (xSemaphoreTake(cfg_state.lock, CFG_DELAY) != pdTRUE)
    {
        const char *err_str = "Error taking mutex.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        result = -1;
        goto err_out;
    }

    if (cfg_state.state > cfg_state_idle)
    {
        const char *err_str = "Already connecting.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        result = -1;
        goto err_out;
    }

#ifndef DEMO_MODE
    ESP_LOGI(TAG, "[%s] Starting WPS.", __FUNCTION__);

    /* Save current config for fall-back. */
    result = get_wifi_cfg(&cfg);
    if (result != ESP_OK)
    {
        const char *err_str = "Error fetching WiFi config.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        goto err_out;
    }

    memmove(&cfg_state.saved, &cfg, sizeof(cfg_state.saved));
    cfg_state.state = cfg_state_wps_start;

    if (xTimerChangePeriod(config_timer, CFG_DELAY, CFG_DELAY) != pdTRUE)
    {
        cfg_state.state = cfg_state_failed;
        result = -1;
        goto err_out;
    }
#else
    {
        const char *err_str = "Demo mode, not starting WPS.";
        ESP_LOGW(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "demo", err_str);
        result = ESP_OK; // fake OK status for demo
    }
#endif

err_out:
    xSemaphoreGive(cfg_state.lock);
    cJSON_AddBoolToObject(jsroot, "success", (result == ESP_OK));
    return cgiJsonResponseCommonSingle(connData, jsroot); // Send the json response!
}

/* CGI for get/set settings in AP mode.
 * See API spec: /README-wifi_api.md
 */
CgiStatus cgiWiFiAPSettings(HttpdConnData *connData)
{
    if (connData->isConnectionClosed)
    {
        /* Connection aborted. Clean up. */
        return HTTPD_CGI_DONE;
    }

    if (connData->requestType != HTTPD_METHOD_GET && connData->requestType != HTTPD_METHOD_POST)
    {
        return HTTPD_CGI_NOTFOUND;
    }

    // Pointer to the args, could be either from GET or POST
    char *allArgs = connData->getArgs;
    if (connData->requestType == HTTPD_METHOD_POST)
    {
        allArgs = connData->post.buff;
    }

    char arg_buf[ARGBUFSIZE];
    esp_err_t result = ESP_OK;
    cJSON *jsroot = cJSON_CreateObject();

    // Get the current settings
    struct wifi_cfg cfg;
    result = get_wifi_cfg(&cfg);
    if (result != ESP_OK)
    {
        const char *err_str = "Error fetching WiFi config.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        goto err_out;
    }

    // Get the args
    bool has_arg_chan = false;
    unsigned int chan;
    if (cgiGetArgDecU32(allArgs, "chan", &chan, arg_buf, sizeof(arg_buf)))
    {
        if (chan < 1 || chan > 15)
        {
            ESP_LOGW(TAG, "[%s] Invalid channel %s", __FUNCTION__, arg_buf);
        }
        else
        {
            has_arg_chan = true;
        }
    }

    bool has_arg_ssid = false;
    char ssid[32]; /**< SSID of ESP32 soft-AP */
    if (cgiGetArgString(allArgs, "ssid", ssid, sizeof(ssid)))
    {
        has_arg_ssid = true;
    }

    bool has_arg_pass = false;
    char pass[64]; /**< Password of ESP32 soft-AP */
    if (cgiGetArgString(allArgs, "pass", pass, sizeof(pass)))
    {

        has_arg_pass = true;
    }

    // Do the command
    if (has_arg_chan)
    {
        ESP_LOGI(TAG, "[%s] Setting ch=%d", __FUNCTION__, chan);
        cfg.ap.ap.channel = (uint8)chan;
    }
    cJSON_AddNumberToObject(jsroot, "chan", cfg.ap.ap.channel);

    if (has_arg_ssid)
    {
        ESP_LOGI(TAG, "[%s] Setting ssid=%s", __FUNCTION__, ssid);
        strlcpy((char *)cfg.ap.ap.ssid, ssid, sizeof(cfg.ap.ap.ssid));
        cfg.ap.ap.ssid_len = 0; // if ssid_len==0, check the SSID until there is a termination character; otherwise, set the SSID length according to softap_config.ssid_len.
    }
    cJSON_AddStringToObject(jsroot, "ssid", (char *)cfg.ap.ap.ssid);

    if (has_arg_pass)
    {
        ESP_LOGI(TAG, "[%s] Setting pass=%s", __FUNCTION__, pass);
        strlcpy((char *)cfg.ap.ap.password, pass, sizeof(cfg.ap.ap.password));
    }
    cJSON_AddStringToObject(jsroot, "pass", (char *)cfg.ap.ap.password);

    bool enabled = cfg.mode == WIFI_MODE_AP || cfg.mode == WIFI_MODE_APSTA;
    cJSON_AddBoolToObject(jsroot, "enabled", enabled);

    // Only commit the change if one or more args is passed.  Otherwise just return the current settings.
    if (has_arg_chan || has_arg_ssid || has_arg_pass)
    {
#ifndef DEMO_MODE
        result = update_wifi(&cfg_state, &cfg, false);
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] Setting WiFi config failed", __FUNCTION__);
        }
#else
        {
            const char *err_str = "Demo mode, not changing AP settings.";
            ESP_LOGW(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "demo", err_str);
            result = ESP_OK; // fake OK status for demo
        }
#endif
    }

err_out:
    cJSON_AddBoolToObject(jsroot, "success", (result == ESP_OK));
    return cgiJsonResponseCommonSingle(connData, jsroot); // Send the json response!
}

/* CGI returning the current state of the WiFi STA connection to an AP.
 * See API spec: /README-wifi_api.md
 */
CgiStatus cgiWiFiConnStatus(HttpdConnData *connData)
{
    if (connData->isConnectionClosed)
    {
        /* Connection aborted. Clean up. */
        return HTTPD_CGI_DONE;
    }

    if (connData->requestType != HTTPD_METHOD_GET)
    {
        return HTTPD_CGI_NOTFOUND;
    }

    esp_err_t result = ESP_OK;
    cJSON *jsroot = cJSON_CreateObject();
    struct wifi_cfg cfg;

    memset(&cfg, 0x0, sizeof(cfg));
    result = get_wifi_cfg(&cfg);
    if (result != ESP_OK)
    {
        const char *err_str = "Error fetching WiFi config.";
        ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
        cJSON_AddStringToObject(jsroot, "error", err_str);
        goto err_out;
    }
    bool working = false;

    bool enabled = cfg.mode == WIFI_MODE_STA || cfg.mode == WIFI_MODE_APSTA;
    wifi_sta_config_t *sta = &(cfg.sta.sta);
    cJSON_AddStringToObject(jsroot, "ssid", (char *)sta->ssid);
    cJSON_AddStringToObject(jsroot, "pass", (char *)sta->password);
    cJSON_AddBoolToObject(jsroot, "enabled", enabled);

    if (!enabled)
    {
        cJSON_AddStringToObject(jsroot, "error", "STA disabled");
    }
    else
    {
        switch (cfg_state.state)
        {
        case cfg_state_idle:
            // working = false;
            break;
        case cfg_state_update:
        case cfg_state_connecting:
        case cfg_state_wps_start:
        case cfg_state_wps_active:
            working = true;
            break;
        case cfg_state_connected:
            // working = false;
            break;
        case cfg_state_failed:
        default:
            cJSON_AddStringToObject(jsroot, "error", "cfg_state_failed");
            // working = false;
            break;
        }
    }
    cJSON_AddBoolToObject(jsroot, "working", working);

    bool connected = sta_connected();
    if (connected)
    {
        esp_netif_t *netif_sta = NULL;
        esp_netif_ip_info_t sta_ip_info;
        char ip_str_buf[IP4ADDR_STRLEN_MAX];
        char *ip_str = NULL;
        netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif_sta != NULL && esp_netif_get_ip_info(netif_sta, &sta_ip_info) == ESP_OK)
        {
            ip_str = ip4addr_ntoa_r((ip4_addr_t *)&sta_ip_info.ip, ip_str_buf, sizeof(ip_str_buf));
        }
        if (ip_str != NULL)
        {
            cJSON_AddStringToObject(jsroot, "ip", ip_str); // AddString duplicates the string, so ok to use buffer on stack
        }
        else
        {
            const char *err_str = "Error fetching IP config.";
            ESP_LOGE(TAG, "[%s] %s", __FUNCTION__, err_str);
            cJSON_AddStringToObject(jsroot, "error", err_str);
            goto err_out;
        }
    }

    cJSON_AddBoolToObject(jsroot, "connected", connected);

err_out:
    cJSON_AddBoolToObject(jsroot, "success", (result == ESP_OK));
    return cgiJsonResponseCommonSingle(connData, jsroot); // Send the json response!
}

#endif // ESP32
