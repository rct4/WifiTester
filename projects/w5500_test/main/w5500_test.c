#include <stdio.h>
#include <string.h>
#include <inttypes.h>

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

#define PING_TARGET "8.8.8.8"

static const char *TAG = "eth_tester";

static int transmitted = 0;
static int received = 0;
static uint32_t last_rtt_ms = 0;
static float jitter_ms = 0.0f;
static uint32_t min_rtt = UINT32_MAX;
static uint32_t max_rtt = 0;
static uint64_t total_rtt = 0;

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

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));

    start_ping();
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint32_t seqno;
    uint32_t ttl;
    uint32_t rtt_ms;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt_ms, sizeof(rtt_ms));

    transmitted++;
    received++;

    if (rtt_ms < min_rtt) min_rtt = rtt_ms;
    if (rtt_ms > max_rtt) max_rtt = rtt_ms;
    total_rtt += rtt_ms;

    if (last_rtt_ms != 0) {
        uint32_t diff = (rtt_ms > last_rtt_ms)
            ? (rtt_ms - last_rtt_ms)
            : (last_rtt_ms - rtt_ms);

        jitter_ms = 0.8f * jitter_ms + 0.2f * diff;
    }

    last_rtt_ms = rtt_ms;

    float loss_percent = 100.0f * (transmitted - received) / transmitted;
    float avg_rtt = (received > 0) ? ((float)total_rtt / received) : 0.0f;

    ESP_LOGI(
        TAG,
        "seq=%" PRIu32 " rtt=%" PRIu32 " ms ttl=%" PRIu32
        " | avg=%.2f ms min=%" PRIu32 " max=%" PRIu32
        " | jitter=%.2f ms loss=%.1f%%",
        seqno,
        rtt_ms,
        ttl,
        avg_rtt,
        min_rtt,
        max_rtt,
        jitter_ms,
        loss_percent
    );
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint32_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));

    transmitted++;

    float loss_percent = 100.0f * (transmitted - received) / transmitted;

    ESP_LOGW(
        TAG,
        "seq=%" PRIu32 " timeout | loss=%.1f%%",
        seqno,
        loss_percent
    );
}

static void start_ping(void)
{
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

    ESP_LOGI(TAG, "Started network test: pinging %s", PING_TARGET);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_MISO,
        .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .spics_io_num = PIN_CS,
        .queue_size = 20,
    };

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);

    w5500_config.int_gpio_num = -1;
    w5500_config.poll_period_ms = 100;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = PIN_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC/PHY");
        return;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    ESP_ERROR_CHECK(esp_netif_attach(
        eth_netif,
        esp_eth_new_netif_glue(eth_handle)
    ));

    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT,
        ESP_EVENT_ANY_ID,
        eth_event_handler,
        NULL
    ));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_ETH_GOT_IP,
        got_ip_event_handler,
        NULL
    ));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "W5500 Ethernet network tester started");
}