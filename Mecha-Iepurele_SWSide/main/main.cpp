#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"

static const char *TAG = "ESP32_S3_INMP441_STT";

/* Wi-Fi Router Credentials */
#define WIFI_SSID          ""
#define WIFI_PASS          ""

/* OpenAI API Key Macro */
#define OPENAI_API_KEY     ""

#define UART_PORT_NUM      UART_NUM_1
#define UART_BAUD_RATE     115200
#define UART_TX_PIN        GPIO_NUM_43
#define UART_RX_PIN        GPIO_NUM_44
#define BUF_SIZE           1024

#define I2S_SCK_PIN        GPIO_NUM_4
#define I2S_WS_PIN         GPIO_NUM_5
#define I2S_SD_PIN         GPIO_NUM_21

#define SAMPLE_RATE        16000
#define RECORD_TIME_SEC    30
#define TOTAL_SAMPLES      (SAMPLE_RATE * RECORD_TIME_SEC)
#define TOTAL_PCM_BYTES    (TOTAL_SAMPLES * sizeof(int16_t))
#define CHUNK_SAMPLES      512

#define VAD_SPEECH_THRESHOLD 400
#define AUDIO_GAIN_FACTOR    16

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* Buffere globale persistente în PSRAM pentru acces concomitent (Whisper & Web Server) */
static int16_t *g_audio_psram_buf = NULL;
static size_t g_last_recorded_samples = 0;
static SemaphoreHandle_t g_audio_mutex = NULL;

typedef struct __attribute__((packed)) {
    char     riff_header[4];    // "RIFF"
    uint32_t wav_size;          // Total size - 8
    char     wave_header[4];    // "WAVE"
    char     fmt_header[4];     // "fmt "
    uint32_t fmt_chunk_size;    // 16
    uint16_t audio_format;      // 1 (PCM)
    uint16_t num_channels;      // 1 (Mono)
    uint32_t sample_rate;       // 16000
    uint32_t byte_rate;         // sample_rate * 1 * 2 = 32000
    uint16_t block_align;       // 1 * 2 = 2
    uint16_t bits_per_sample;   // 16
    char     data_header[4];    // "data"
    uint32_t data_bytes;        // pcm_bytes
} wav_header_t;

static wav_header_t create_wav_header(uint32_t pcm_bytes, uint32_t sample_rate)
{
    wav_header_t header;
    memcpy(header.riff_header, "RIFF", 4);
    header.wav_size = pcm_bytes + 36;
    memcpy(header.wave_header, "WAVE", 4);
    memcpy(header.fmt_header, "fmt ", 4);
    header.fmt_chunk_size = 16;
    header.audio_format = 1;
    header.num_channels = 1;
    header.sample_rate = sample_rate;
    header.byte_rate = sample_rate * 1 * sizeof(int16_t);
    header.block_align = 1 * sizeof(int16_t);
    header.bits_per_sample = 16;
    memcpy(header.data_header, "data", 4);
    header.data_bytes = pcm_bytes;
    return header;
}

static i2s_chan_handle_t init_inmp441_i2s(void)
{
    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 24,
        .dma_frame_num = 512,
        .auto_clear = true,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S INMP441 inițializat cu succes.");
    return rx_handle;
}

static bool record_and_evaluate_speech(i2s_chan_handle_t rx_handle)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " Ascultare Sound Trigger (VAD: %d, Max: %ds)...", VAD_SPEECH_THRESHOLD, RECORD_TIME_SEC);
    ESP_LOGI(TAG, "=================================================");

    int32_t *raw_chunk = (int32_t *)malloc(CHUNK_SAMPLES * sizeof(int32_t));
    if (!raw_chunk || !g_audio_psram_buf) {
        ESP_LOGE(TAG, "Eroare: Lipsă memorie pentru captură!");
        if (raw_chunk) free(raw_chunk);
        return false;
    }

    esp_task_wdt_add(NULL);

    size_t dummy_bytes = 0;
    i2s_channel_read(rx_handle, raw_chunk, CHUNK_SAMPLES * sizeof(int32_t), &dummy_bytes, pdMS_TO_TICKS(100));

    static int32_t dc_offset = 0;
    size_t samples_captured = 0;

    /* 1. Așteptare Voice Trigger */
    bool sound_detected = false;
    while (!sound_detected) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_handle, raw_chunk, CHUNK_SAMPLES * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK && bytes_read > 0) {
            size_t read_samples = bytes_read / sizeof(int32_t);
            int64_t chunk_sum_abs = 0;

            for (size_t i = 0; i < read_samples; i++) {
                int16_t val_raw = (int16_t)(raw_chunk[i] >> 16);
                dc_offset = (dc_offset * 31 + val_raw) / 32;
                int16_t val = val_raw - (int16_t)dc_offset;

                int32_t val_amp = (int32_t)val * AUDIO_GAIN_FACTOR;
                if (val_amp > 32767) val_amp = 32767;
                else if (val_amp < -32768) val_amp = -32768;

                g_audio_psram_buf[i] = (int16_t)val_amp;
                chunk_sum_abs += (val_amp < 0) ? -val_amp : val_amp;
            }

            int chunk_avg_amp = (int)(chunk_sum_abs / read_samples);
            if (chunk_avg_amp >= VAD_SPEECH_THRESHOLD) {
                sound_detected = true;
                samples_captured = read_samples;
                ESP_LOGI(TAG, "Sunet detectat (Avg: %d >= %d)! Înregistrare în curs...",
                         chunk_avg_amp, VAD_SPEECH_THRESHOLD);
                break;
            }
        }
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            ESP_LOGI(TAG, "Buton BOOT apăsat!");
            break;
        }
        esp_task_wdt_reset();
    }

    if (!sound_detected) {
        free(raw_chunk);
        esp_task_wdt_delete(NULL);
        return false;
    }

    /* 2. Înregistrare 100% în PSRAM */
    int silence_chunk_counter = 0;
    bool recording = true;

    while (recording && samples_captured < TOTAL_SAMPLES) {
        size_t samples_to_read = CHUNK_SAMPLES;
        if (samples_captured + samples_to_read > TOTAL_SAMPLES) {
            samples_to_read = TOTAL_SAMPLES - samples_captured;
        }

        size_t bytes_to_read = samples_to_read * sizeof(int32_t);
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(rx_handle, raw_chunk, bytes_to_read, &bytes_read, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK && bytes_read > 0) {
            size_t read_samples = bytes_read / sizeof(int32_t);
            int64_t chunk_sum_abs = 0;

            for (size_t i = 0; i < read_samples; i++) {
                int16_t val_raw = (int16_t)(raw_chunk[i] >> 16);
                dc_offset = (dc_offset * 31 + val_raw) / 32;
                int16_t val = val_raw - (int16_t)dc_offset;

                int32_t val_amp = (int32_t)val * AUDIO_GAIN_FACTOR;
                if (val_amp > 32767) val_amp = 32767;
                else if (val_amp < -32768) val_amp = -32768;

                g_audio_psram_buf[samples_captured++] = (int16_t)val_amp;
                chunk_sum_abs += (val_amp < 0) ? -val_amp : val_amp;
            }

            int chunk_avg_amp = (int)(chunk_sum_abs / read_samples);

            if (gpio_get_level(GPIO_NUM_0) == 0) {
                ESP_LOGI(TAG, "Oprire manuală prin buton BOOT.");
                break;
            }

            if (chunk_avg_amp >= VAD_SPEECH_THRESHOLD) {
                silence_chunk_counter = 0;
            } else {
                silence_chunk_counter++;
                /* 62 chunk-uri @ 512 samples = ~1.98s liniște */
                if (silence_chunk_counter >= 62) {
                    ESP_LOGI(TAG, "Liniște 2.0s detectată! Înregistrare gata (%zu samples).", samples_captured);
                    recording = false;
                    break;
                }
            }
        }
        esp_task_wdt_reset();
    }

    esp_task_wdt_delete(NULL);
    free(raw_chunk);

    /* 3. Actualizăm starea bufferului partajat fără să atingem flash-ul */
    xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    g_last_recorded_samples = samples_captured;
    xSemaphoreGive(g_audio_mutex);

    ESP_LOGI(TAG, "Audio pregătit instantaneu în RAM: %zu samples (%.2f secunde, %zu bytes PCM)",
             samples_captured, (double)samples_captured / SAMPLE_RATE, samples_captured * sizeof(int16_t));

    return (samples_captured > 0);
}

/* Transmisie directă din PSRAM către OpenAI Whisper prin HTTP POST */
static void send_audio_to_openai_whisper_ram(void)
{
    xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    size_t samples_count = g_last_recorded_samples;
    int16_t *audio_data = g_audio_psram_buf;
    xSemaphoreGive(g_audio_mutex);

    if (samples_count == 0 || !audio_data) {
        ESP_LOGE(TAG, "Nu există date audio în RAM de trimis către Whisper!");
        return;
    }

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " Trimitere directă din PSRAM către OpenAI Whisper (%zu samples)...", samples_count);
    ESP_LOGI(TAG, "=================================================");

    uint32_t pcm_bytes = samples_count * sizeof(int16_t);
    wav_header_t wav_hdr = create_wav_header(pcm_bytes, SAMPLE_RATE);
    uint32_t file_size = sizeof(wav_header_t) + pcm_bytes;

    char *response_buf = (char *)malloc(2048);
    if (!response_buf) {
        ESP_LOGE(TAG, "Alocare eșuată pentru bufferul de răspuns HTTP!");
        return;
    }

    const char *boundary = "ESP32Boundary7MA4YWxk";

    const char *model_header =
        "--ESP32Boundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-1\r\n";

    const char *language_header =
        "--ESP32Boundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "ro\r\n";

    const char *file_header =
        "--ESP32Boundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"speech_direct.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";

    const char *crlf = "\r\n";
    const char *footer = "--ESP32Boundary7MA4YWxk--\r\n";

    int total_len = strlen(model_header) + 
                    strlen(language_header) + 
                    strlen(file_header) + 
                    file_size + 
                    strlen(crlf) + 
                    strlen(footer);

    esp_http_client_config_t config = {};
    config.url = "https://api.openai.com/v1/audio/transcriptions";
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 45000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Inițializare HTTP Client eșuată!");
        free(response_buf);
        return;
    }

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", OPENAI_API_KEY);

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "User-Agent", "ESP32S3-Whisper/2.0");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Deschidere conexiune HTTPS eșuată: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(response_buf);
        return;
    }

    /* 1. Scriem câmpurile text */
    esp_http_client_write(client, model_header, strlen(model_header));
    esp_http_client_write(client, language_header, strlen(language_header));
    esp_http_client_write(client, file_header, strlen(file_header));

    /* 2. Scriem header-ul WAV generat în zbor */
    esp_http_client_write(client, (const char *)&wav_hdr, sizeof(wav_header_t));

    /* 3. Transmitem întregul buffer din PSRAM în bucăți de 4096 bytes */
    size_t bytes_sent = 0;
    const char *raw_ptr = (const char *)audio_data;

    while (bytes_sent < pcm_bytes) {
        size_t to_send = (pcm_bytes - bytes_sent > 4096) ? 4096 : (pcm_bytes - bytes_sent);
        int written = esp_http_client_write(client, raw_ptr + bytes_sent, to_send);
        if (written < 0) {
            ESP_LOGE(TAG, "Eroare la scrierea socket-ului SSL la offset %zu!", bytes_sent);
            break;
        }
        bytes_sent += written;
    }

    /* 4. Scriem terminatorul multipart */
    esp_http_client_write(client, crlf, strlen(crlf));
    esp_http_client_write(client, footer, strlen(footer));

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "OpenAI HTTP Response Code: %d", status_code);

    int read_len = esp_http_client_read_response(client, response_buf, 2047);
    if (read_len >= 0) {
        response_buf[read_len] = '\0';
        ESP_LOGI(TAG, "====================================================================");
        ESP_LOGI(TAG, " OPENAI WHISPER RESULT: %s", response_buf);
        ESP_LOGI(TAG, "====================================================================");

        uart_write_bytes(UART_PORT_NUM, response_buf, read_len);
        uart_write_bytes(UART_PORT_NUM, "\n", 1);
    } else {
        ESP_LOGE(TAG, "Eroare la citirea răspunsului de la OpenAI!");
    }

    esp_http_client_cleanup(client);
    free(response_buf);
}

/* Handler Web: descarcă audio-ul direct din PSRAM generând headerul WAV */
static esp_err_t wav_download_handler(httpd_req_t *req)
{
    xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    size_t samples_count = g_last_recorded_samples;
    int16_t *audio_data = g_audio_psram_buf;

    if (samples_count == 0 || !audio_data) {
        xSemaphoreGive(g_audio_mutex);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Nu există nicio înregistrare disponibilă în RAM.");
        return ESP_FAIL;
    }

    uint32_t pcm_bytes = samples_count * sizeof(int16_t);
    wav_header_t wav_hdr = create_wav_header(pcm_bytes, SAMPLE_RATE);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"speech_verify.wav\"");

    /* Trimite mai întâi header-ul de 44 bytes */
    httpd_resp_send_chunk(req, (const char *)&wav_hdr, sizeof(wav_header_t));

    /* Trimite datele audio din PSRAM */
    const char *raw_ptr = (const char *)audio_data;
    size_t sent = 0;
    while (sent < pcm_bytes) {
        size_t chunk = (pcm_bytes - sent > 4096) ? 4096 : (pcm_bytes - sent);
        if (httpd_resp_send_chunk(req, raw_ptr + sent, chunk) != ESP_OK) {
            xSemaphoreGive(g_audio_mutex);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
        sent += chunk;
    }

    xSemaphoreGive(g_audio_mutex);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Fișierul WAV a fost descărcat integral din PSRAM către browser!");
    return ESP_OK;
}

static const char html_page[] =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32-S3 PSRAM Live Audio</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px 20px;margin:0;}"
    ".card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:30px;max-width:480px;margin:0 auto;box-shadow:0 8px 24px rgba(0,0,0,0.5);}"
    "h1{color:#58a6ff;font-size:22px;margin-bottom:8px;}"
    "audio{width:100%;margin:25px 0;outline:none;}"
    ".btn{display:inline-block;background:#238636;color:#ffffff;text-decoration:none;padding:12px 24px;border-radius:6px;font-weight:bold;font-size:15px;}"
    ".info{background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:14px;margin-top:25px;font-size:13px;color:#8b949e;text-align:left;line-height:1.6;}"
    "</style></head><body><div class=\"card\">"
    "<h1>ESP32-S3 Live Voice Player</h1>"
    "<p>Stream direct din PSRAM (Zero Flash Delays)</p>"
    "<audio controls src=\"/download\"></audio>"
    "<div><a href=\"/download\" download=\"speech_verify.wav\" class=\"btn\">Download speech_verify.wav</a></div>"
    "<div class=\"info\">"
    "&bull; <b>Sursă:</b> PSRAM RAM Buffer<br>"
    "&bull; <b>Format:</b> 16,000 Hz, 16-bit Mono PCM<br>"
    "&bull; <b>Latență salvare disc:</b> 0 ms"
    "</div></div></body></html>";

static esp_err_t html_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = html_index_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t download_uri = { .uri = "/download", .method = HTTP_GET, .handler = wav_download_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &download_uri);

        return server;
    }
    return NULL;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi deconectat. Reconectare...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "====================================================================");
        ESP_LOGI(TAG, " WI-FI CONECTAT: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, " PLAYER WEB     : http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "====================================================================");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Wi-Fi Timeout!");
    }
}

static void sync_sntp_time(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) != ESP_OK && ++retry < 10) {
        ESP_LOGI(TAG, "Sincronizare SNTP... (%d/10)", retry);
    }
}

extern "C" void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    g_audio_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Memorie SPIRAM liberă: %d bytes (~%d MB)",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));

    /* Alocare permanentă buffer audio de 30 secunde în PSRAM (960 KB) */
    g_audio_psram_buf = (int16_t *)heap_caps_malloc(TOTAL_PCM_BYTES, MALLOC_CAP_SPIRAM);
    if (!g_audio_psram_buf) {
        ESP_LOGE(TAG, "CRITIC: Nu s-a putut aloca bufferul de 30s în PSRAM!");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));

    wifi_init_sta();
    sync_sntp_time();

    i2s_chan_handle_t rx_handle = init_inmp441_i2s();
    start_webserver();

    char tx_json[256];
    uint8_t rx_buf[BUF_SIZE];
    int seq_num = 1;

    while (1)
    {
        ESP_LOGI(TAG, "Ciclul #%d pregătit...", seq_num);

        bool has_speech = record_and_evaluate_speech(rx_handle);

        if (has_speech) {
            ESP_LOGI(TAG, "Vorbire detectată! Transmitere instantanee din PSRAM către Whisper...");
            send_audio_to_openai_whisper_ram();
        }

        snprintf(tx_json, sizeof(tx_json),
                 "{\"cmd\":\"motor_control\",\"left_speed\":100,\"right_speed\":100,\"servo_angle\":90,\"seq\":%d}\n",
                 seq_num);

        uart_flush_input(UART_PORT_NUM);
        uart_write_bytes(UART_PORT_NUM, tx_json, strlen(tx_json));

        int len = uart_read_bytes(UART_PORT_NUM, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(1000));
        if (len > 0) {
            rx_buf[len] = '\0';
        }

        seq_num++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
