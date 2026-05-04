#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "nvs_flash.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_SCLK 18
#define PIN_CS   5
#define PIN_RST  4

#define PING_TARGET "192.168.50.1"   // start with router

#define TEST_DURATION_SECONDS 60
#define PING_INTERVAL_MS 1000

static const char *TAG = "eth_tester";

static int transmitted = 0;
static int received = 0;

static uint32_t last_rtt = 0;
static uint32_t min_rtt = UINT32_MAX;
static uint32_t max_rtt = 0;
static uint64_t total_rtt = 0;

static uint64_t total_jitter = 0;
static int jitter_samples = 0;

static void start_ping(void);

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet Link Down");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;

    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));

    // Wait to ensure routing is ready
    vTaskDelay(pdMS_TO_TICKS(2000));

    start_ping();
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint32_t rtt;
    uint32_t seqno;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));

    transmitted++;
    received++;

    total_rtt += rtt;

    if (rtt < min_rtt) min_rtt = rtt;
    if (rtt > max_rtt) max_rtt = rtt;

    if (last_rtt != 0) {
        uint32_t diff = abs((int)rtt - (int)last_rtt);
        total_jitter += diff;
        jitter_samples++;
    }

    last_rtt = rtt;

    float avg_rtt = (float) total_rtt / received;
    float avg_jitter = jitter_samples ? (float) total_jitter / jitter_samples : 0;
    float loss = 100.0f * (transmitted - received) / transmitted;

    ESP_LOGI(TAG,
        "seq=%d rtt=%d ms | avg=%.2f min=%d max=%d | jitter=%.2f | loss=%.1f%%",
        seqno, rtt, avg_rtt, min_rtt, max_rtt, avg_jitter, loss);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint32_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));

    transmitted++;

    float loss = 100.0f * (transmitted - received) / transmitted;

    ESP_LOGW(TAG, "timeout seq=%d | loss=%.1f%%", seqno, loss);
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    float avg_rtt = received ? (float) total_rtt / received : 0;
    float avg_jitter = jitter_samples ? (float) total_jitter / jitter_samples : 0;
    float loss = transmitted ? 100.0f * (transmitted - received) / transmitted : 0;

    ESP_LOGI(TAG, "========== FINAL RESULT ==========");
    ESP_LOGI(TAG, "avg RTT: %.2f ms", avg_rtt);
    ESP_LOGI(TAG, "min RTT: %d ms", min_rtt);
    ESP_LOGI(TAG, "max RTT: %d ms", max_rtt);
    ESP_LOGI(TAG, "avg jitter: %.2f ms", avg_jitter);
    ESP_LOGI(TAG, "loss: %.1f%%", loss);
    ESP_LOGI(TAG, "=================================");

    esp_ping_delete_session(hdl);
}

static void start_ping(void)
{
    ip_addr_t target;
    inet_pton(AF_INET, PING_TARGET, &target.u_addr.ip4);
    target.type = IPADDR_TYPE_V4;

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = TEST_DURATION_SECONDS;
    config.interval_ms = PING_INTERVAL_MS;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end
    };

    esp_ping_handle_t ping;
    ESP_ERROR_CHECK(esp_ping_new_session(&config, &cbs, &ping));
    ESP_ERROR_CHECK(esp_ping_start(ping));

    ESP_LOGI(TAG, "Ping started");
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&cfg);

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_MISO,
        .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCLK
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_CS
    };

    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    w5500.int_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &(eth_mac_config_t){});
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&(eth_phy_config_t){ .reset_gpio_num = PIN_RST });

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle;

    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &handle));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(handle));
}