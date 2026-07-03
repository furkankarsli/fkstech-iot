#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "driver/i2c.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "cJSON.h"

#define SERVER_URL     "https://fkstech.site/pico/data"
#define STATUS_URL     "https://fkstech.site/status"
#define API_KEY        "tasarim_projesi_secret_key"
#define UDP_PORT       8266

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "ESP32_IoT";

// wifi kurulum: acilista kayitli aglari sirayla dener, yoksa kendi hotspotunu acar
#define WIFI_PROV_NS   "wifiprov"
#define WIFI_PROV_MAX  5

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_net_t;

static bool s_auto_reconnect = false;
static wifi_ap_record_t s_scan_records[20];
static uint16_t s_scan_count = 0;

static int wifi_prov_load(wifi_net_t *nets, int max)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_PROV_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t count = 0;
    nvs_get_u8(h, "count", &count);
    if (count > max) count = max;
    for (int i = 0; i < count; i++) {
        char key[16];
        size_t len;
        snprintf(key, sizeof(key), "ssid%d", i);
        len = sizeof(nets[i].ssid);
        if (nvs_get_str(h, key, nets[i].ssid, &len) != ESP_OK) nets[i].ssid[0] = '\0';
        snprintf(key, sizeof(key), "pass%d", i);
        len = sizeof(nets[i].pass);
        if (nvs_get_str(h, key, nets[i].pass, &len) != ESP_OK) nets[i].pass[0] = '\0';
    }
    nvs_close(h);
    return count;
}

static void wifi_prov_save(const char *ssid, const char *pass)
{
    wifi_net_t nets[WIFI_PROV_MAX];
    int count = wifi_prov_load(nets, WIFI_PROV_MAX);

    int found = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(nets[i].ssid, ssid) == 0) { found = i; break; }
    }
    if (found >= 0) {
        strncpy(nets[found].pass, pass, sizeof(nets[found].pass) - 1);
        nets[found].pass[sizeof(nets[found].pass) - 1] = '\0';
    } else {
        if (count >= WIFI_PROV_MAX) {
            for (int i = 1; i < count; i++) nets[i - 1] = nets[i];
            count = WIFI_PROV_MAX - 1;
        }
        strncpy(nets[count].ssid, ssid, sizeof(nets[count].ssid) - 1);
        nets[count].ssid[sizeof(nets[count].ssid) - 1] = '\0';
        strncpy(nets[count].pass, pass, sizeof(nets[count].pass) - 1);
        nets[count].pass[sizeof(nets[count].pass) - 1] = '\0';
        count++;
    }

    nvs_handle_t h;
    if (nvs_open(WIFI_PROV_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "count", (uint8_t)count);
    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ssid%d", i);
        nvs_set_str(h, key, nets[i].ssid);
        snprintf(key, sizeof(key), "pass%d", i);
        nvs_set_str(h, key, nets[i].pass);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP Alindi: " IPSTR, IP2STR(&event->ip_info.ip));

        esp_netif_dns_info_t dns;
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
        dns.ip.type = IPADDR_TYPE_V4;
        esp_netif_set_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns);

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_core_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void wifi_do_scan(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        s_scan_count = 0;
        return;
    }
    uint16_t num = sizeof(s_scan_records) / sizeof(s_scan_records[0]);
    if (esp_wifi_scan_get_ap_records(&num, s_scan_records) != ESP_OK) {
        s_scan_count = 0;
        return;
    }
    s_scan_count = num;
}

static bool wifi_scan_has(const char *ssid)
{
    for (int i = 0; i < s_scan_count; i++) {
        if (strcmp((char*)s_scan_records[i].ssid, ssid) == 0) return true;
    }
    return false;
}

static bool wifi_try_connect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t cfg = { 0 };
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    if (esp_wifi_connect() != ESP_OK) return false;

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) return true;

    esp_wifi_disconnect();
    return false;
}

static bool wifi_provision_connect(void)
{
    wifi_net_t nets[WIFI_PROV_MAX];
    int count = wifi_prov_load(nets, WIFI_PROV_MAX);
    if (count == 0) return false;

    wifi_do_scan();

    for (int i = 0; i < count; i++) {
        if (!wifi_scan_has(nets[i].ssid)) continue;
        ESP_LOGI(TAG, "WiFi deneniyor: %s", nets[i].ssid);
        if (wifi_try_connect(nets[i].ssid, nets[i].pass, 15000)) {
            ESP_LOGI(TAG, "WiFi baglandi: %s", nets[i].ssid);
            s_auto_reconnect = true;
            return true;
        }
        ESP_LOGW(TAG, "Baglanamadi (15sn): %s", nets[i].ssid);
    }
    return false;
}

static const char *PORTAL_HEAD =
    "<!DOCTYPE html><html lang='tr'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WiFi Kurulum</title><style>"
    "body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;padding:18px;max-width:460px;margin:0 auto}"
    "h2{color:#58a6ff}label{font-size:.85rem;color:#8b949e}"
    "input,select{width:100%;padding:11px;margin:6px 0 14px;border-radius:6px;border:1px solid #30363d;background:#161b22;color:#fff;box-sizing:border-box}"
    "button{width:100%;padding:13px;background:#238636;color:#fff;border:0;border-radius:6px;font-weight:700;font-size:1rem}"
    "</style></head><body><h2>ESP32 WiFi Kurulum</h2><form method='POST' action='/save'>"
    "<label>Ag Secin</label><select name='ssid'>";

static const char *PORTAL_TAIL =
    "</select><label>Sifre</label><input type='password' name='pass' placeholder='WiFi sifresi'>"
    "<button type='submit'>Kaydet ve Baglan</button></form>"
    "<p style='color:#8b949e;font-size:.75rem'>Kaydedilen aglar hafizada tutulur, sonraki aciliste otomatik denenir.</p>"
    "</body></html>";

static void url_decode(const char *src, char *dst, int dstsize)
{
    int di = 0;
    for (int i = 0; src[i] != '\0' && di < dstsize - 1; i++) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

static void parse_form(const char *body, const char *key, char *out, int outsize)
{
    out[0] = '\0';
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return;
    p += strlen(pat);
    const char *end = strchr(p, '&');
    int len = end ? (int)(end - p) : (int)strlen(p);
    char raw[128];
    if (len >= (int)sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(raw, out, outsize);
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    char *html = malloc(4096);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int off = snprintf(html, 4096, "%s", PORTAL_HEAD);
    for (int i = 0; i < s_scan_count && off < 3600; i++) {
        off += snprintf(html + off, 4096 - off, "<option value=\"%s\">%s (%d dBm)</option>",
                        (char*)s_scan_records[i].ssid, (char*)s_scan_records[i].ssid,
                        s_scan_records[i].rssi);
    }
    snprintf(html + off, 4096 - off, "%s", PORTAL_TAIL);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char buf[256];
    int total = req->content_len;
    if (total >= (int)sizeof(buf)) total = sizeof(buf) - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) break;
        received += r;
    }
    buf[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    parse_form(buf, "ssid", ssid, sizeof(ssid));
    parse_form(buf, "pass", pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "SSID bos olamaz");
        return ESP_OK;
    }

    wifi_prov_save(ssid, pass);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
        "<body style='font-family:sans-serif;background:#0d1117;color:#c9d1d9;padding:18px'>"
        "<h2 style='color:#3fb950'>Kaydedildi</h2><p>Cihaz yeniden baslatiliyor...</p></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void dns_server_task(void *pv)
{
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (len < 12) continue;

        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;

        int idx = len;
        if (idx + 16 > (int)sizeof(buf)) continue;
        buf[idx++] = 0xC0; buf[idx++] = 0x0C;
        buf[idx++] = 0x00; buf[idx++] = 0x01;
        buf[idx++] = 0x00; buf[idx++] = 0x01;
        buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 0x3C;
        buf[idx++] = 0x00; buf[idx++] = 0x04;
        buf[idx++] = 192; buf[idx++] = 168; buf[idx++] = 4; buf[idx++] = 1;

        sendto(sock, buf, idx, 0, (struct sockaddr *)&client, clen);
    }
}

static void wifi_start_portal(void)
{
    ESP_LOGW(TAG, "Kurulum modu: 'ESP32-Kurulum' agina baglanip tarayicidan ayar yapin.");

    wifi_do_scan();

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = { 0 };
    strncpy((char*)ap_cfg.ap.ssid, "ESP32-Kurulum", sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen("ESP32-Kurulum");
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    vTaskDelay(pdMS_TO_TICKS(200));

    esp_netif_dns_info_t dns_info = { 0 };
    dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    dns_info.ip.type = IPADDR_TYPE_V4;
    uint8_t dhcps_offer_dns = 2;
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_dns, sizeof(dhcps_offer_dns));
    esp_netif_dhcps_start(ap_netif);

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = portal_get_handler };
        httpd_register_uri_handler(server, &root);
        httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = portal_save_handler };
        httpd_register_uri_handler(server, &save);
        httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = portal_redirect_handler };
        httpd_register_uri_handler(server, &any);
    }

    xTaskCreate(dns_server_task, "dns_server", 3072, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// tum gorevlerin paylastigi sensor ve sistem durumu (mutex korumali)
typedef struct {

    float temperature;
    float humidity;
    int gas;
    int flame;

    double current_mA;
    int door;

    char oled_mod[16];
    char oled_mesaj[32];
    bool guvenlik_aktif;
} sensor_data_t;

static sensor_data_t g_sensor_data = {
    .temperature = 24.5,
    .humidity = 45.0,
    .gas = 120,
    .flame = 0,
    .current_mA = 0.0,
    .door = 1,
    .oled_mod = "alfabe",
    .oled_mesaj = "Hazir",
    .guvenlik_aktif = true
};

static SemaphoreHandle_t g_sensor_mutex = NULL;

// sunucu ile http (keep-alive): durum cekme ve toplu veri gonderme
typedef struct {
    char *buffer;
    int len;
    int max;
} http_resp_t;

static esp_err_t status_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        http_resp_t *resp = (http_resp_t *)evt->user_data;
        int copy_len = evt->data_len;
        if (resp->len + copy_len > resp->max - 1) {
            copy_len = (resp->max - 1) - resp->len;
        }
        if (copy_len > 0) {
            memcpy(resp->buffer + resp->len, evt->data, copy_len);
            resp->len += copy_len;
            resp->buffer[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

static void check_status_and_control_led(esp_http_client_handle_t client, http_resp_t *resp)
{
    resp->len = 0;
    resp->buffer[0] = '\0';

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET Hatasi: %s", esp_err_to_name(err));
        return;
    }
    if (resp->len <= 0) {
        return;
    }

    ESP_LOGI(TAG, "Status Alindi: %s", resp->buffer);

    cJSON *root = cJSON_Parse(resp->buffer);
    if (root) {
        cJSON *oled_mod = cJSON_GetObjectItem(root, "oled_mod");
        cJSON *oled_mesaj = cJSON_GetObjectItem(root, "oled_mesaj");
        cJSON *guvenlik_aktif = cJSON_GetObjectItem(root, "guvenlik_aktif");

        if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (oled_mod && cJSON_IsString(oled_mod)) {
                strncpy(g_sensor_data.oled_mod, oled_mod->valuestring, sizeof(g_sensor_data.oled_mod) - 1);
            }
            if (oled_mesaj && cJSON_IsString(oled_mesaj)) {
                strncpy(g_sensor_data.oled_mesaj, oled_mesaj->valuestring, sizeof(g_sensor_data.oled_mesaj) - 1);
            }
            if (guvenlik_aktif && cJSON_IsBool(guvenlik_aktif)) {
                g_sensor_data.guvenlik_aktif = cJSON_IsTrue(guvenlik_aktif);
            }
            xSemaphoreGive(g_sensor_mutex);
        }

        cJSON_Delete(root);
    }
}

// 8x8 font ve oled surucu (sh1106, i2c)
static const uint8_t font8x8_basic_upper[26][8] = {
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}
};

#define I2C_MASTER_SCL_IO           25
#define I2C_MASTER_SDA_IO           26
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          400000
#define OLED_I2C_ADDRESS            0x3C

static void i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

static esp_err_t oled_send_cmd(uint8_t cmd) {
    i2c_cmd_handle_t cmd_link = i2c_cmd_link_create();
    i2c_master_start(cmd_link);
    i2c_master_write_byte(cmd_link, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_link, 0x00, true);
    i2c_master_write_byte(cmd_link, cmd, true);
    i2c_master_stop(cmd_link);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_link, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd_link);
    return ret;
}

static void oled_init(void) {
    oled_send_cmd(0xAE);
    oled_send_cmd(0xD5);
    oled_send_cmd(0x80);
    oled_send_cmd(0xA8);
    oled_send_cmd(0x3F);
    oled_send_cmd(0xD3);
    oled_send_cmd(0x00);
    oled_send_cmd(0x40);
    oled_send_cmd(0xAD);
    oled_send_cmd(0x8B);
    oled_send_cmd(0x8D);
    oled_send_cmd(0x14);
    oled_send_cmd(0xA1);
    oled_send_cmd(0xC8);
    oled_send_cmd(0xDA);
    oled_send_cmd(0x12);
    oled_send_cmd(0x81);
    oled_send_cmd(0xCF);
    oled_send_cmd(0xD9);
    oled_send_cmd(0xF1);
    oled_send_cmd(0xDB);
    oled_send_cmd(0x40);
    oled_send_cmd(0xA4);
    oled_send_cmd(0xA6);
    oled_send_cmd(0xAF);
}

static void oled_clear(uint8_t pattern) {
    for (uint8_t page = 0; page < 8; page++) {
        oled_send_cmd(0xB0 + page);
        oled_send_cmd(0x02);
        oled_send_cmd(0x10);

        i2c_cmd_handle_t cmd_link = i2c_cmd_link_create();
        i2c_master_start(cmd_link);
        i2c_master_write_byte(cmd_link, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd_link, 0x40, true);
        for (int i = 0; i < 128; i++) {
            i2c_master_write_byte(cmd_link, pattern, true);
        }
        i2c_master_stop(cmd_link);
        i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_link, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd_link);
    }
}

static void oled_draw_char_normal(uint8_t page, uint8_t col, char c, bool invert) {
    uint8_t glyph[8] = {0};
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if (c >= 'A' && c <= 'Z') {
        uint8_t char_idx = c - 'A';
        for (int col_idx = 0; col_idx < 8; col_idx++) {
            uint8_t col_byte = 0;
            for (int row = 0; row < 8; row++) {
                uint8_t bit = (font8x8_basic_upper[char_idx][row] >> col_idx) & 1;
                col_byte |= (bit << row);
            }
            glyph[col_idx] = invert ? ~col_byte : col_byte;
        }
    } else {

        for (int col_idx = 0; col_idx < 8; col_idx++) {
            glyph[col_idx] = invert ? 0xFF : 0x00;
        }
    }

    uint8_t actual_col = col + 2;
    oled_send_cmd(0xB0 + page);
    oled_send_cmd(0x00 + (actual_col & 0x0F));
    oled_send_cmd(0x10 + ((actual_col >> 4) & 0x0F));

    i2c_cmd_handle_t cmd_link = i2c_cmd_link_create();
    i2c_master_start(cmd_link);
    i2c_master_write_byte(cmd_link, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_link, 0x40, true);
    for (int i = 0; i < 8; i++) {
        i2c_master_write_byte(cmd_link, glyph[i], true);
    }
    i2c_master_stop(cmd_link);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_link, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd_link);
}

static void oled_draw_string(uint8_t page, uint8_t start_col, const char *str, bool invert) {
    uint8_t col = start_col;
    while (*str) {
        oled_draw_char_normal(page, col, *str, invert);
        col += 8;
        if (col > 120) break;
        str++;
    }
}

static void oled_draw_string_centered(uint8_t page, const char *str, bool invert) {
    int len = strlen(str);
    if (len > 16) len = 16;
    uint8_t start_col = (128 - (len * 8)) / 2;
    uint8_t margin_val = invert ? 0xFF : 0x00;

    oled_send_cmd(0xB0 + page);
    oled_send_cmd(0x02);
    oled_send_cmd(0x10);
    i2c_cmd_handle_t cmd_link = i2c_cmd_link_create();
    i2c_master_start(cmd_link);
    i2c_master_write_byte(cmd_link, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_link, 0x40, true);
    for (int i = 0; i < start_col; i++) {
        i2c_master_write_byte(cmd_link, margin_val, true);
    }
    i2c_master_stop(cmd_link);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_link, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd_link);

    oled_draw_string(page, start_col, str, invert);

    uint8_t end_col = start_col + (len * 8);
    uint8_t actual_end_col = end_col + 2;
    oled_send_cmd(0xB0 + page);
    oled_send_cmd(0x00 + (actual_end_col & 0x0F));
    oled_send_cmd(0x10 + ((actual_end_col >> 4) & 0x0F));
    cmd_link = i2c_cmd_link_create();
    i2c_master_start(cmd_link);
    i2c_master_write_byte(cmd_link, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_link, 0x40, true);
    for (int i = end_col; i < 128; i++) {
        i2c_master_write_byte(cmd_link, margin_val, true);
    }
    i2c_master_stop(cmd_link);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_link, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd_link);
}

// oled uzerinde surekli donen kayan yazi
#define MARQUEE_MAX_COLS 1200
static uint8_t g_marquee_cols[MARQUEE_MAX_COLS];
static int g_marquee_len = 0;

static void marquee_char_columns(char c, uint8_t cols[8]) {
    static const uint8_t glyph_dash[8]  = {0x00, 0x00, 0x00, 0x3C, 0x3C, 0x00, 0x00, 0x00};
    static const uint8_t glyph_colon[8] = {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00};
    const uint8_t *rows = NULL;

    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if (c >= 'A' && c <= 'Z') {
        rows = font8x8_basic_upper[c - 'A'];
    } else if (c == '-') {
        rows = glyph_dash;
    } else if (c == ':') {
        rows = glyph_colon;
    } else {
        for (int i = 0; i < 8; i++) cols[i] = 0x00;
        return;
    }
    for (int col = 0; col < 8; col++) {
        uint8_t b = 0;
        for (int row = 0; row < 8; row++) {
            b |= (uint8_t)(((rows[row] >> col) & 1) << row);
        }
        cols[col] = b;
    }
}

static void marquee_build(const char *txt) {
    int idx = 0;
    for (int i = 0; txt[i] != '\0'; i++) {
        uint8_t cols[8];
        marquee_char_columns(txt[i], cols);
        for (int c = 0; c < 8 && idx < MARQUEE_MAX_COLS; c++) {
            g_marquee_cols[idx++] = cols[c];
        }
        if (idx < MARQUEE_MAX_COLS) g_marquee_cols[idx++] = 0x00;
    }
    for (int g = 0; g < 128 && idx < MARQUEE_MAX_COLS; g++) {
        g_marquee_cols[idx++] = 0x00;
    }
    g_marquee_len = idx;
}

static void oled_write_page_columns(uint8_t page, const uint8_t *cols) {
    oled_send_cmd(0xB0 + page);
    oled_send_cmd(0x02);
    oled_send_cmd(0x10);

    i2c_cmd_handle_t link = i2c_cmd_link_create();
    i2c_master_start(link);
    i2c_master_write_byte(link, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(link, 0x40, true);
    for (int i = 0; i < 128; i++) {
        i2c_master_write_byte(link, cols[i], true);
    }
    i2c_master_stop(link);
    i2c_master_cmd_begin(I2C_MASTER_NUM, link, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(link);
}

static void marquee_draw(int pos) {
    static uint8_t top[128];
    static uint8_t bot[128];
    if (g_marquee_len <= 0) return;

    for (int x = 0; x < 128; x++) {
        int src = (pos + x) % g_marquee_len;
        uint8_t b = g_marquee_cols[src];
        uint8_t t = 0, u = 0;
        for (int i = 0; i < 4; i++) {
            if (b & (1 << i))       t |= (uint8_t)(0x03 << (2 * i));
            if (b & (1 << (i + 4))) u |= (uint8_t)(0x03 << (2 * i));
        }
        top[x] = t;
        bot[x] = u;
    }
    oled_write_page_columns(3, top);
    oled_write_page_columns(4, bot);
}

void server_sync_task(void *pvParameters)
{

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    static char resp_buf[512];
    http_resp_t resp = { .buffer = resp_buf, .len = 0, .max = sizeof(resp_buf) };

    esp_http_client_config_t config = {
        .url = STATUS_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = status_http_event_handler,
        .user_data = &resp,
        .keep_alive_enable = true,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-API-Key", API_KEY);

    while (1) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            check_status_and_control_led(client, &resp);
        } else {
            ESP_LOGW(TAG, "Wi-Fi baglantisi bekleniyor, durum sorgulanmadi.");
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void http_post_task(void *pvParameters)
{

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .keep_alive_enable = true,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-API-Key", API_KEY);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (!(bits & WIFI_CONNECTED_BIT)) {
            continue;
        }

        float temp = 24.5;
        float hum = 45.0;
        int gas = 120;
        int flame = 0;
        double akim = 0.0;
        int door = 1;

        if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            temp = g_sensor_data.temperature;
            hum = g_sensor_data.humidity;
            gas = g_sensor_data.gas;
            flame = g_sensor_data.flame;
            akim = g_sensor_data.current_mA;
            door = g_sensor_data.door;
            xSemaphoreGive(g_sensor_mutex);
        }

        char post_data[512];
        snprintf(post_data, sizeof(post_data),
                 "[{\"istasyon_id\":1,\"gaz\":%d,\"alev\":%d,\"sicaklik_C\":%.2f,\"nem_Yuzde\":%.2f},"
                 "{\"istasyon_id\":2,\"akim_mA\":%.2f,\"kapi\":\"%s\"}]",
                 gas, flame, temp, hum,
                 akim, (door == 1) ? "kapali" : "acik");

        esp_http_client_set_post_field(client, post_data, strlen(post_data));
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP POST Basarili, Statu = %d, Veri: %s",
                     esp_http_client_get_status_code(client), post_data);
        } else {
            ESP_LOGE(TAG, "HTTP POST Hatasi: %s", esp_err_to_name(err));
        }
    }
}

void app_task(void *pvParameters)
{
    i2c_master_init();
    oled_init();
    oled_clear(0x00);

    marquee_build("FURKAN KARSLI - AKILLI EV OTOMASYONU - BITIRME CALISMASI - DANISMAN: PROF DR AHMET MERT");
    int marquee_pos = 0;

    bool blink_state = false;
    uint8_t last_display_mode = 99;

    while (1) {

        int gas = 120;
        int flame = 0;
        int door = 1;
        char oled_mod[16] = "alfabe";
        char oled_mesaj[32] = "Hazir";
        bool guvenlik_aktif = true;

        if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            gas = g_sensor_data.gas;
            flame = g_sensor_data.flame;
            door = g_sensor_data.door;
            strncpy(oled_mod, g_sensor_data.oled_mod, sizeof(oled_mod) - 1);
            strncpy(oled_mesaj, g_sensor_data.oled_mesaj, sizeof(oled_mesaj) - 1);
            guvenlik_aktif = g_sensor_data.guvenlik_aktif;
            xSemaphoreGive(g_sensor_mutex);
        }

        bool gas_danger = (gas >= 400);
        bool flame_danger = (flame == 1);
        bool door_danger = (door == 0) && guvenlik_aktif;

        if (gas_danger || flame_danger || door_danger) {

            if (last_display_mode != 3) {
                oled_clear(0x00);
                last_display_mode = 3;
            }

            oled_draw_string_centered(1, "ACIL DURUM", blink_state);
            if (gas_danger) {
                oled_draw_string_centered(3, "GAZ KACAGI", blink_state);
            } else if (flame_danger) {
                oled_draw_string_centered(3, "YANGIN ALARMI", blink_state);
            } else if (door_danger) {
                oled_draw_string_centered(3, "KAPI ACILDI", blink_state);
            }
            oled_draw_string_centered(5, "KONTROL EDIN", blink_state);

            blink_state = !blink_state;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else {

            if (strcmp(oled_mod, "kapali") == 0) {

                if (last_display_mode != 1) {
                    oled_clear(0x00);
                    last_display_mode = 1;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            else if (strcmp(oled_mod, "mesaj") == 0) {

                if (last_display_mode != 2) {
                    oled_clear(0x00);
                    last_display_mode = 2;
                }
                oled_draw_string_centered(1, "SISTEM MESAJI", false);
                oled_draw_string_centered(3, oled_mesaj, true);
                oled_draw_string_centered(5, "FKSTECH SITE", false);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            else {

                if (last_display_mode != 0) {
                    oled_clear(0x00);
                    last_display_mode = 0;
                }
                marquee_draw(marquee_pos);
                marquee_pos += 2;
                if (marquee_pos >= g_marquee_len) marquee_pos -= g_marquee_len;
                vTaskDelay(pdMS_TO_TICKS(40));
            }
        }
    }
}

void udp_server_task(void *pvParameters)
{
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        if (!(bits & WIFI_CONNECTED_BIT)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(UDP_PORT);

        int listen_sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "UDP Socket olusturulamadi: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "UDP Socket olusturuldu (Port: %d)", UDP_PORT);

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "UDP Socket bind edilemedi: errno %d", errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "UDP Socket bind edildi, baglanti dinleniyor...");

        int flags = fcntl(listen_sock, F_GETFL, 0);
        fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK);

        while (1) {
            bits = xEventGroupGetBits(s_wifi_event_group);
            if (!(bits & WIFI_CONNECTED_BIT)) {
                ESP_LOGW(TAG, "UDP Server: Wi-Fi koptu, soket kapatiliyor...");
                break;
            }

            fd_set rfds;
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(listen_sock, &rfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int sret = select(listen_sock + 1, &rfds, NULL, NULL, &tv);
            if (sret < 0) {
                ESP_LOGE(TAG, "select hatasi: errno %d", errno);
                break;
            } else if (sret == 0) {
                continue;
            }

            char temp_buffer[512];
            struct sockaddr_storage source_addr;
            socklen_t socklen = sizeof(source_addr);

            cJSON *latest_node1 = NULL;
            cJSON *latest_node2 = NULL;

            while (1) {
                int len = recvfrom(listen_sock, temp_buffer, sizeof(temp_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
                if (len < 0) {
                    if (errno == EWOULDBLOCK || errno == EAGAIN) {
                        break;
                    }
                    ESP_LOGE(TAG, "recvfrom hatasi: errno %d", errno);
                    break;
                }
                if (len == 0) continue;

                temp_buffer[len] = '\0';
                cJSON *root = cJSON_Parse(temp_buffer);
                if (root != NULL) {
                    cJSON *node_id_item = cJSON_GetObjectItem(root, "node_id");
                    if (node_id_item != NULL && cJSON_IsNumber(node_id_item)) {
                        int node_id = node_id_item->valueint;
                        if (node_id == 1) {
                            if (latest_node1) cJSON_Delete(latest_node1);
                            latest_node1 = root;
                        } else if (node_id == 2) {
                            if (latest_node2) cJSON_Delete(latest_node2);
                            latest_node2 = root;
                        } else {
                            cJSON_Delete(root);
                        }
                    } else {
                        cJSON_Delete(root);
                    }
                }
            }

            if (latest_node1 != NULL) {
                cJSON *temp_item = cJSON_GetObjectItem(latest_node1, "temperature");
                cJSON *hum_item = cJSON_GetObjectItem(latest_node1, "humidity");
                cJSON *gas_item = cJSON_GetObjectItem(latest_node1, "gas");
                cJSON *flame_item = cJSON_GetObjectItem(latest_node1, "flame");

                float temperature = (temp_item && cJSON_IsNumber(temp_item)) ? temp_item->valuedouble : 24.5;
                float humidity = (hum_item && cJSON_IsNumber(hum_item)) ? hum_item->valuedouble : 45.0;
                int gas = (gas_item && cJSON_IsNumber(gas_item)) ? gas_item->valueint : 120;
                int flame = (flame_item && cJSON_IsNumber(flame_item)) ? flame_item->valueint : 0;

                if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_sensor_data.temperature = temperature;
                    g_sensor_data.humidity = humidity;
                    g_sensor_data.gas = gas;
                    g_sensor_data.flame = flame;
                    xSemaphoreGive(g_sensor_mutex);
                }
                ESP_LOGI(TAG, "Dugum 1 Global State Guncellendi (Gas: %d, Flame: %d)", gas, flame);
                cJSON_Delete(latest_node1);
            }

            if (latest_node2 != NULL) {
                cJSON *current_item = cJSON_GetObjectItem(latest_node2, "current_mA");
                cJSON *door_item = cJSON_GetObjectItem(latest_node2, "door");

                double current_mA = (current_item && cJSON_IsNumber(current_item)) ? current_item->valuedouble : 0.0;
                int door = (door_item && cJSON_IsNumber(door_item)) ? door_item->valueint : 1;

                if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_sensor_data.current_mA = current_mA;
                    g_sensor_data.door = door;
                    xSemaphoreGive(g_sensor_mutex);
                }
                ESP_LOGI(TAG, "Dugum 2 Global State Guncellendi (Current: %.2f mA, Door: %d)", current_mA, door);
                cJSON_Delete(latest_node2);
            }
        }

        if (listen_sock != -1) {
            ESP_LOGI(TAG, "Soket kapatiliyor...");
            shutdown(listen_sock, 0);
            close(listen_sock);
        }
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Sistem Baslatiliyor...");

    g_sensor_mutex = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    xTaskCreate(app_task, "app_task", 4096, NULL, 5, NULL);

    wifi_core_init();
    if (!wifi_provision_connect()) {
        wifi_start_portal();
    }
    ESP_LOGI(TAG, "Wi-Fi baglandi, SNTP zaman senkronizasyonu bekleniyor...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    int retry = 0;
    while (now < 1600000000 && ++retry < 30) {
        ESP_LOGI(TAG, "Zaman senkronizasyonu bekleniyor... (%d/30)", retry);
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
    }

    if (now >= 1600000000) {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "Zaman senkronize edildi: %s", strftime_buf);
    } else {
        ESP_LOGW(TAG, "Zaman senkronizasyonu zaman asimina ugradi, HTTPS baglantisi basarisiz olabilir.");
    }

    xTaskCreate(server_sync_task, "server_sync_task", 8192, NULL, 4, NULL);

    xTaskCreate(http_post_task, "http_post_task", 8192, NULL, 4, NULL);

    xTaskCreate(udp_server_task, "udp_server_task", 8192, NULL, 5, NULL);
}
