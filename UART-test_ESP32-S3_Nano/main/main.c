#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ESP32_S3_JSON_UART";

#define UART_PORT_NUM      UART_NUM_1
#define UART_BAUD_RATE     115200
#define UART_TX_PIN        GPIO_NUM_43
#define UART_RX_PIN        GPIO_NUM_44
#define BUF_SIZE           1024

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " ESP32-S3 Nano -> MCXA153 JSON Control Test Suite ");
    ESP_LOGI(TAG, " UART Port: %d (TX: GPIO%d, RX: GPIO%d)", UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN);
    ESP_LOGI(TAG, " Baud Rate: %d 8N1", UART_BAUD_RATE);
    ESP_LOGI(TAG, "=================================================");

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

    /* Flush input buffer to clear startup line noise */
    ESP_ERROR_CHECK(uart_flush_input(UART_PORT_NUM));

    char tx_json[256];
    uint8_t rx_buf[BUF_SIZE];
    int seq_num = 1;

    while (1)
    {
        /* Construct sample JSON motor & servo control command */
        snprintf(tx_json, sizeof(tx_json),
                 "{\"cmd\":\"motor_control\",\"left_speed\":100,\"right_speed\":100,\"servo_angle\":90,\"seq\":%d}\n",
                 seq_num);

        ESP_LOGI(TAG, "[TX JSON Command #%d]: %s", seq_num, tx_json);

        /* Flush input buffer prior to sending request */
        uart_flush_input(UART_PORT_NUM);

        /* Transmit JSON packet over UART */
        uart_write_bytes(UART_PORT_NUM, tx_json, strlen(tx_json));

        /* Read confirmation ACK response from MCXA153 */
        int len = uart_read_bytes(UART_PORT_NUM, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(1000));
        if (len > 0)
        {
            rx_buf[len] = '\0';
            /* Strip trailing carriage returns/newlines for clean logging */
            char *newline = strchr((char *)rx_buf, '\r');
            if (newline) *newline = '\0';
            newline = strchr((char *)rx_buf, '\n');
            if (newline) *newline = '\0';

            ESP_LOGI(TAG, "[RX ACK Response #%d]: %s", seq_num, (char *)rx_buf);

            if (strstr((char *)rx_buf, "\"status\":\"ACK\"") != NULL || strstr((char *)rx_buf, "ACK") != NULL)
            {
                ESP_LOGI(TAG, "[RESULT]: PASS - MCXA153 confirmed receipt of JSON command #%d", seq_num);
            }
            else
            {
                ESP_LOGW(TAG, "[RESULT]: FAIL - Unexpected ACK payload format");
            }
        }
        else
        {
            ESP_LOGW(TAG, "[RESULT]: FAIL - Timeout waiting for ACK from MCXA153");
        }

        seq_num++;
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
