#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#define WIFI_SSID      "Gleshlight "
#define WIFI_PASSWORD  "dickAndBalls1!"

#define PING_TARGET    "192.168.50.1"

#define TEST_DURATION_SECONDS 60
#define PING_INTERVAL_MS 1000

static const char *TAG = "wifi_tester";

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static int transmitted = 0;
static int received = 0;

static uint32_t last_rtt_ms = 0;

static uint32_t min_rtt_ms = UINT32_MAX;
static uint32_t max_rtt_ms = 0;
static uint64_t total_rtt_ms = 0;

static uint64_t total_jitter_diff_ms = 0;
static int jitter_samples = 0;

static int latest_rssi = 0;

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
        wifi_event_sta_disconnected_t *disc =
            (wifi_event_sta_disconnected_t *) event_data;

        ESP_LOGW(TAG, "Disconnected. Reason: %d", disc->reason);

        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
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

static int get_rssi(void) {
    wifi_ap_record_t ap_info;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }

    return 0;
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

    total_rtt_ms += elapsed_time_ms;

    if (elapsed_time_ms < min_rtt_ms) {
        min_rtt_ms = elapsed_time_ms;
    }

    if (elapsed_time_ms > max_rtt_ms) {
        max_rtt_ms = elapsed_time_ms;
    }

    if (last_rtt_ms != 0) {
        uint32_t diff = elapsed_time_ms > last_rtt_ms
            ? elapsed_time_ms - last_rtt_ms
            : last_rtt_ms - elapsed_time_ms;

        total_jitter_diff_ms += diff;
        jitter_samples++;
    }

    last_rtt_ms = elapsed_time_ms;
    latest_rssi = get_rssi();

    float loss_percent = 0.0f;
    if (transmitted > 0) {
        loss_percent = 100.0f * (transmitted - received) / transmitted;
    }

    float avg_rtt = 0.0f;
    if (received > 0) {
        avg_rtt = (float) total_rtt_ms / received;
    }

    float avg_jitter = 0.0f;
    if (jitter_samples > 0) {
        avg_jitter = (float) total_jitter_diff_ms / jitter_samples;
    }

    ESP_LOGI(
        TAG,
        "Reply seq=%" PRIu32 " time=%" PRIu32 " ms ttl=%" PRIu32
        " | avg=%.2f ms min=%" PRIu32 " max=%" PRIu32
        " | avg_jitter=%.2f ms | loss=%.1f%% | RSSI=%d dBm",
        seqno,
        elapsed_time_ms,
        ttl,
        avg_rtt,
        min_rtt_ms,
        max_rtt_ms,
        avg_jitter,
        loss_percent,
        latest_rssi
    );
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    uint32_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));

    transmitted++;
    latest_rssi = get_rssi();

    float loss_percent = 100.0f * (transmitted - received) / transmitted;

    ESP_LOGW(
        TAG,
        "Timeout seq=%" PRIu32 " | loss=%.1f%% | RSSI=%d dBm",
        seqno,
        loss_percent,
        latest_rssi
    );
}

static void on_ping_end(esp_ping_handle_t hdl, void *args) {
    float loss_percent = 0.0f;
    float avg_rtt = 0.0f;
    float avg_jitter = 0.0f;

    if (transmitted > 0) {
        loss_percent = 100.0f * (transmitted - received) / transmitted;
    }

    if (received > 0) {
        avg_rtt = (float) total_rtt_ms / received;
    }

    if (jitter_samples > 0) {
        avg_jitter = (float) total_jitter_diff_ms / jitter_samples;
    }

    latest_rssi = get_rssi();

    ESP_LOGI(TAG, "================ FINAL NETWORK TEST RESULT ================");
    ESP_LOGI(TAG, "Target: %s", PING_TARGET);
    ESP_LOGI(TAG, "Duration: %d seconds", TEST_DURATION_SECONDS);
    ESP_LOGI(TAG, "Packets transmitted: %d", transmitted);
    ESP_LOGI(TAG, "Packets received: %d", received);
    ESP_LOGI(TAG, "Packet loss: %.1f%%", loss_percent);

    if (received > 0) {
        ESP_LOGI(TAG, "Average RTT: %.2f ms", avg_rtt);
        ESP_LOGI(TAG, "Minimum RTT: %" PRIu32 " ms", min_rtt_ms);
        ESP_LOGI(TAG, "Maximum RTT: %" PRIu32 " ms", max_rtt_ms);
        ESP_LOGI(TAG, "Average jitter: %.2f ms", avg_jitter);
    } else {
        ESP_LOGW(TAG, "No successful ping replies received.");
    }

    ESP_LOGI(TAG, "Final RSSI: %d dBm", latest_rssi);
    ESP_LOGI(TAG, "===========================================================");

    esp_ping_delete_session(hdl);
}

static void start_ping(void) {
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));

    inet_pton(AF_INET, PING_TARGET, &target_addr.u_addr.ip4);
    target_addr.type = IPADDR_TYPE_V4;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;

    ping_config.count = TEST_DURATION_SECONDS;
    ping_config.interval_ms = PING_INTERVAL_MS;
    ping_config.timeout_ms = 1000;
    ping_config.data_size = 32;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end,
        .cb_args = NULL,
    };

    esp_ping_handle_t ping;
    ESP_ERROR_CHECK(esp_ping_new_session(&ping_config, &callbacks, &ping));
    ESP_ERROR_CHECK(esp_ping_start(ping));

    ESP_LOGI(TAG, "Started %d-second ping test to %s", TEST_DURATION_SECONDS, PING_TARGET);
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