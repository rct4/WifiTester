#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#define WIFI_SSID      "Hotspottest"
#define WIFI_PASSWORD  "iloveece"

#define PING_TARGET    "8.8.8.8"

static const char *TAG = "wifi_tester";

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static int transmitted = 0;
static int received = 0;
static uint32_t last_rtt_ms = 0;
static float jitter_ms = 0.0f;

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected. Reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        NULL
    ));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        NULL
    ));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");

    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI(TAG, "Wi-Fi connected.");
}

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint32_t elapsed_time_ms;
    uint32_t seqno;
    uint32_t ttl;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time_ms, sizeof(elapsed_time_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));

    transmitted++;
    received++;

    if (last_rtt_ms != 0) {
        uint32_t diff = elapsed_time_ms > last_rtt_ms
            ? elapsed_time_ms - last_rtt_ms
            : last_rtt_ms - elapsed_time_ms;

        jitter_ms = 0.8f * jitter_ms + 0.2f * diff;
    }

    last_rtt_ms = elapsed_time_ms;

    wifi_ap_record_t ap_info;
    int rssi = 0;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    float loss_percent = 0.0f;
    if (transmitted > 0) {
        loss_percent = 100.0f * (transmitted - received) / transmitted;
    }

    ESP_LOGI(
        TAG,
        "Reply seq=%" PRIu32 " time=%" PRIu32 " ms ttl=%" PRIu32
        " | jitter=%.2f ms | loss=%.1f%% | RSSI=%d dBm",
        seqno,
        elapsed_time_ms,
        ttl,
        jitter_ms,
        loss_percent,
        rssi
    );
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    uint32_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));

    transmitted++;

    wifi_ap_record_t ap_info;
    int rssi = 0;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    float loss_percent = 100.0f * (transmitted - received) / transmitted;

    ESP_LOGW(
        TAG,
        "Timeout seq=%" PRIu32 " | loss=%.1f%% | RSSI=%d dBm",
        seqno,
        loss_percent,
        rssi
    );
}

static void start_ping(void) {
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));

    inet_pton(AF_INET, PING_TARGET, &target_addr.u_addr.ip4);
    target_addr.type = IPADDR_TYPE_V4;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = ESP_PING_COUNT_INFINITE;
    ping_config.interval_ms = 1000;
    ping_config.timeout_ms = 1000;
    ping_config.data_size = 32;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = NULL,
        .cb_args = NULL,
    };

    esp_ping_handle_t ping;
    ESP_ERROR_CHECK(esp_ping_new_session(&ping_config, &callbacks, &ping));
    ESP_ERROR_CHECK(esp_ping_start(ping));

    ESP_LOGI(TAG, "Started ping test to %s", PING_TARGET);
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    start_ping();
}