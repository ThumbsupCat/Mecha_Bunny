#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_spiffs.h"
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

static const char *TAG = "ESP32_S3_INMP441_TEST";

/* Wi-Fi Router Credentials - Edit these macros to match your Wi-Fi AP */
#define WIFI_SSID          "DIGI-h89Y"
#define WIFI_PASS          "dkmXJ2qb"

/* OpenAI API Key Macro - Edit this macro to specify your OpenAI API Key */
#define OPENAI_API_KEY     "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"

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

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

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
    uint32_t data_bytes;        // TOTAL_PCM_BYTES
} wav_header_t;

static wav_header_t create_wav_header(uint32_t pcm_bytes, uint32_t sample_rate)
{
    wav_header_t header;
    memcpy(header.riff_header, "RIFF", 4);
    header.wav_size = pcm_bytes + sizeof(wav_header_t) - 8;
    memcpy(header.wave_header, "WAVE", 4);
    memcpy(header.fmt_header, "fmt ", 4);
    header.fmt_chunk_size = 16;
    header.audio_format = 1; // PCM
    header.num_channels = 1; // Mono
    header.sample_rate = sample_rate;
    header.byte_rate = sample_rate * 1 * sizeof(int16_t);
    header.block_align = 1 * sizeof(int16_t);
    header.bits_per_sample = 16;
    memcpy(header.data_header, "data", 4);
    header.data_bytes = pcm_bytes;
    return header;
}

static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS storage...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format SPIFFS filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition in partition table");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted successfully. Storage total: %u bytes, used: %u bytes", (unsigned int)total, (unsigned int)used);
    }
    return ESP_OK;
}

static i2s_chan_handle_t init_inmp441_i2s(void)
{
    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
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

    /* INMP441 uses left channel when L/R pin is connected to GND */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S INMP441 initialized successfully (SCK: GPIO%d, WS: GPIO%d, SD: GPIO%d)",
             I2S_SCK_PIN, I2S_WS_PIN, I2S_SD_PIN);

    return rx_handle;
}

static bool record_and_evaluate_speech(i2s_chan_handle_t rx_handle)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " Listening for Speech (Auto-stops after 2.0s silence)...");
    ESP_LOGI(TAG, "=================================================");

    FILE *f = fopen("/spiffs/speech_10s.wav", "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open /spiffs/speech_10s.wav for writing!");
        return false;
    }

    /* Reserve 44-byte space for WAV header */
    wav_header_t wav_hdr = create_wav_header(TOTAL_PCM_BYTES, SAMPLE_RATE);
    fwrite(&wav_hdr, sizeof(wav_header_t), 1, f);

    /* Dynamically allocate chunk buffers from heap SRAM */
    int32_t *raw_chunk = (int32_t *)malloc(CHUNK_SAMPLES * sizeof(int32_t));
    int16_t *pcm_chunk = (int16_t *)malloc(CHUNK_SAMPLES * sizeof(int16_t));
    if (!raw_chunk || !pcm_chunk) {
        ESP_LOGE(TAG, "Failed to allocate SRAM for chunk buffers!");
        if (raw_chunk) free(raw_chunk);
        if (pcm_chunk) free(pcm_chunk);
        fclose(f);
        return false;
    }

    /* Subscribe task to Task Watchdog */
    esp_task_wdt_add(NULL);

    size_t samples_captured = 0;
    int64_t sum_sq = 0;
    int16_t min_val = 32767;
    int16_t max_val = -32768;

    bool speech_detected = false;
    int silence_chunk_counter = 0;

    while (samples_captured < TOTAL_SAMPLES) {
        size_t samples_to_read = CHUNK_SAMPLES;
        if (samples_captured + samples_to_read > TOTAL_SAMPLES) {
            samples_to_read = TOTAL_SAMPLES - samples_captured;
        }

        size_t bytes_to_read = samples_to_read * sizeof(int32_t);
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(rx_handle, raw_chunk, bytes_to_read, &bytes_read, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK && bytes_read > 0) {
            size_t read_samples = bytes_read / sizeof(int32_t);
            int64_t chunk_sum_sq = 0;
            int64_t chunk_sum_abs = 0;

            for (size_t i = 0; i < read_samples; i++) {
                int16_t val = (int16_t)(raw_chunk[i] >> 16);
                pcm_chunk[i] = val;

                if (val < min_val) min_val = val;
                if (val > max_val) max_val = val;

                chunk_sum_sq += (int64_t)val * val;
                chunk_sum_abs += (val < 0) ? -val : val;
            }

            int chunk_avg_amp = (int)(chunk_sum_abs / read_samples);
            sum_sq += chunk_sum_sq;

            fwrite(pcm_chunk, sizeof(int16_t), read_samples, f);
            samples_captured += read_samples;

            /* Check BOOT button manual stop override (GPIO0 active low) */
            if (gpio_get_level(GPIO_NUM_0) == 0) {
                ESP_LOGI(TAG, "BOOT button pressed! Stopping recording early (%zu samples).", samples_captured);
                break;
            }

            /* Voice Activity Detection (VAD) using Average Absolute Amplitude */
            if (chunk_avg_amp >= 100) {
                if (!speech_detected) {
                    speech_detected = true;
                    ESP_LOGI(TAG, "Speech active! (Avg amplitude: %d)", chunk_avg_amp);
                }
                silence_chunk_counter = 0;
            } else {
                silence_chunk_counter++;
                /* 62 chunks @ 512 samples = ~31,744 samples = ~2.0 seconds of silence */
                if (silence_chunk_counter >= 62) {
                    if (speech_detected) {
                        ESP_LOGI(TAG, "Silence detected for 2.0s after speech! Auto-stopping recording (%zu samples).", samples_captured);
                    } else {
                        ESP_LOGI(TAG, "No speech detected within 2.0s window. Auto-stopping recording.");
                    }
                    break;
                }
            }
        } else {
            ESP_LOGW(TAG, "I2S read warning / timeout");
        }

        /* Reset Task Watchdog */
        esp_task_wdt_reset();
    }

    esp_task_wdt_delete(NULL);

    free(raw_chunk);
    free(pcm_chunk);

    /* Rewind file and update WAV header with exact recorded length */
    uint32_t actual_pcm_bytes = samples_captured * sizeof(int16_t);
    wav_hdr = create_wav_header(actual_pcm_bytes, SAMPLE_RATE);
    fseek(f, 0, SEEK_SET);
    fwrite(&wav_hdr, sizeof(wav_header_t), 1, f);
    fclose(f);

    ESP_LOGI(TAG, "Recording finalized! Saved %zu samples (%.2f sec, %u bytes) to /spiffs/speech_10s.wav",
             samples_captured, (double)samples_captured / SAMPLE_RATE, (unsigned int)actual_pcm_bytes);

    return speech_detected;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected. Retrying connection...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "====================================================================");
        ESP_LOGI(TAG, " SUCCESS! ESP32-S3 WI-FI CONNECTED TO: %s", WIFI_SSID);
        ESP_LOGI(TAG, " ESP32 IP ADDRESS                    : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, " WEB INTERFACE (Audio Player & Download): http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, " DIRECT WAV FILE DOWNLOAD             : http://" IPSTR "/download", IP2STR(&event->ip_info.ip));
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

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID '%s'...", WIFI_SSID);

    /* Wait for Wi-Fi IP address acquisition up to 15 seconds */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi station initialization completed successfully.");
    } else {
        ESP_LOGE(TAG, "====================================================================");
        ESP_LOGE(TAG, " WI-FI CONNECTION TIMEOUT / FAILED!");
        ESP_LOGE(TAG, " Please set your WIFI_SSID and WIFI_PASS in main.c to your Wi-Fi AP!");
        ESP_LOGE(TAG, "====================================================================");
    }
}

static const char html_page[] =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32-S3 Speech Audio Interface</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px 20px;margin:0;}"
    ".card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:30px;max-width:480px;margin:0 auto;box-shadow:0 8px 24px rgba(0,0,0,0.5);}"
    "h1{color:#58a6ff;font-size:22px;margin-bottom:8px;}"
    "p{color:#8b949e;font-size:14px;margin-top:0;}"
    "audio{width:100%;margin:25px 0;outline:none;}"
    ".btn{display:inline-block;background:#238636;color:#ffffff;text-decoration:none;padding:12px 24px;border-radius:6px;font-weight:bold;font-size:15px;transition:0.2s;}"
    ".btn:hover{background:#2ea043;}"
    ".info{background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:14px;margin-top:25px;font-size:13px;color:#8b949e;text-align:left;line-height:1.6;}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1>ESP32-S3 INMP441 Audio Player</h1>"
    "<p>10-Second Speech Recording</p>"
    "<audio controls src=\"/download\"></audio>"
    "<div><a href=\"/download\" download=\"speech_10s.wav\" class=\"btn\">Download speech_10s.wav</a></div>"
    "<div class=\"info\">"
    "<b>Recording Specs:</b><br>"
    "&bull; Audio Format: PCM 16-bit Mono WAV<br>"
    "&bull; Sample Rate: 16,000 Hz<br>"
    "&bull; Duration: 10.0 Seconds<br>"
    "&bull; File Path: /spiffs/speech_10s.wav"
    "</div></div></body></html>";

static esp_err_t html_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wav_download_handler(httpd_req_t *req)
{
    FILE *file = fopen("/spiffs/speech_10s.wav", "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open /spiffs/speech_10s.wav for HTTP download!");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "WAV speech recording not found on SPIFFS storage.");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"speech_10s.wav\"");

    char buffer[1024];
    size_t read_bytes;

    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(file);
            ESP_LOGE(TAG, "HTTP WAV file streaming aborted!");
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "HTTP WAV speech file downloaded successfully by web client!");
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP Web File Server on port %d...", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = html_index_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t download_uri = {
            .uri       = "/download",
            .method    = HTTP_GET,
            .handler   = wav_download_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &download_uri);

        return server;
    }

    ESP_LOGE(TAG, "Failed to start HTTP Web Server!");
    return NULL;
}

static void sync_sntp_time(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " Synchronizing System Clock via NTP (pool.ntp.org)... ");
    ESP_LOGI(TAG, "=================================================");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    int retry = 0;
    const int retry_count = 10;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) != ESP_OK && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    }

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "System time synchronized: %s", asctime(&timeinfo));
}

static void send_audio_to_openai_whisper(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " Sending /spiffs/speech_10s.wav to OpenAI Whisper STT... ");
    ESP_LOGI(TAG, "=================================================");

    FILE *f = fopen("/spiffs/speech_10s.wav", "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open /spiffs/speech_10s.wav for STT transmission!");
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *chunk = (char *)malloc(1024);
    char *response_buf = (char *)malloc(2048);
    if (!chunk || !response_buf) {
        ESP_LOGE(TAG, "Failed to allocate SRAM for HTTP STT buffers!");
        if (chunk) free(chunk);
        if (response_buf) free(response_buf);
        fclose(f);
        return;
    }

    const char *boundary = "ESP32Boundary";

    const char *model_header =
        "--ESP32Boundary\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n"
        "\r\n"
        "whisper-1\r\n";

    const char *file_header =
        "--ESP32Boundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"speech_10s.wav\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n";

    const char *footer =
        "\r\n"
        "--ESP32Boundary--\r\n";

    int total_len = strlen(model_header) + strlen(file_header) + file_size + strlen(footer);

    esp_http_client_config_t config = {};
    config.url = "https://api.openai.com/v1/audio/transcriptions";
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 30000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for OpenAI!");
        free(chunk);
        free(response_buf);
        fclose(f);
        return;
    }

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", OPENAI_API_KEY);

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "User-Agent", "ESP32S3-OpenAI-Client/1.0");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to OpenAI: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(chunk);
        free(response_buf);
        fclose(f);
        return;
    }

    esp_http_client_write(client, model_header, strlen(model_header));
    esp_http_client_write(client, file_header, strlen(file_header));

    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, 1024, f)) > 0) {
        esp_http_client_write(client, chunk, read_bytes);
    }
    fclose(f);

    esp_http_client_write(client, footer, strlen(footer));

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "OpenAI HTTP Response Code: %d (Content Length: %d)", status_code, content_length);

    int read_len = esp_http_client_read_response(client, response_buf, 2047);

    if (read_len >= 0) {
        response_buf[read_len] = '\0';
        ESP_LOGI(TAG, "====================================================================");
        ESP_LOGI(TAG, " OPENAI WHISPER TRANSCRIPTION RESULT: ");
        ESP_LOGI(TAG, " %s", response_buf);
        ESP_LOGI(TAG, "====================================================================");
    } else {
        ESP_LOGE(TAG, "Failed to read response payload from OpenAI API!");
    }

    esp_http_client_cleanup(client);
    free(chunk);
    free(response_buf);
}

extern "C" void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " ESP32-S3 Nano -> INMP441 & MCXA153 Test Suite   ");
    ESP_LOGI(TAG, " UART Port: %d (TX: GPIO%d, RX: GPIO%d)", UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN);
    ESP_LOGI(TAG, " I2S Mic  : INMP441 (SCK: GPIO%d, WS: GPIO%d, SD: GPIO%d)", I2S_SCK_PIN, I2S_WS_PIN, I2S_SD_PIN);
    ESP_LOGI(TAG, "=================================================");

    /* Initialize NVS Flash (required for Wi-Fi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize SPIFFS Filesystem */
    init_spiffs();

    /* Configure BOOT button (GPIO0) as input with internal pull-up */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Configure UART parameters */
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
    ESP_ERROR_CHECK(uart_flush_input(UART_PORT_NUM));

    /* Connect to Wi-Fi & sync NTP time first at startup */
    wifi_init_sta();
    sync_sntp_time();

    /* Initialize INMP441 I2S Channel & Web Server */
    i2s_chan_handle_t rx_handle = init_inmp441_i2s();
    start_webserver();

    char tx_json[256];
    uint8_t rx_buf[BUF_SIZE];
    int seq_num = 1;

    while (1)
    {
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, " Starting Speech Capture & Prompt Cycle #%d...", seq_num);
        ESP_LOGI(TAG, "=================================================");

        /* 1. Record speech from microphone (auto-stops on 2.0s silence / 30s cap / BOOT button) */
        bool has_speech = record_and_evaluate_speech(rx_handle);

        /* 2. Upload recorded audio to OpenAI Whisper API only if speech was detected */
        if (has_speech) {
            ESP_LOGI(TAG, "Speech detected. Uploading recording to OpenAI Whisper API...");
            send_audio_to_openai_whisper();
        } else {
            ESP_LOGI(TAG, "Environment silent. Skipping OpenAI API upload.");
        }

        /* 3. Construct and transmit JSON motor/servo command packet to MCXA153 over UART */
        snprintf(tx_json, sizeof(tx_json),
                 "{\"cmd\":\"motor_control\",\"left_speed\":100,\"right_speed\":100,\"servo_angle\":90,\"seq\":%d}\n",
                 seq_num);

        uart_flush_input(UART_PORT_NUM);
        uart_write_bytes(UART_PORT_NUM, tx_json, strlen(tx_json));

        /* Read ACK response from MCXA153 over UART */
        int len = uart_read_bytes(UART_PORT_NUM, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(1000));
        if (len > 0)
        {
            rx_buf[len] = '\0';
        }

        seq_num++;
        ESP_LOGI(TAG, "Cycle #%d complete. Starting next speech recording in 1s...", seq_num - 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}



