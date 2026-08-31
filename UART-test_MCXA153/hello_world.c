/*
 * Copyright (c) 2013 - 2015, Freescale Semiconductor, Inc.
 * Copyright 2016-2017, 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "fsl_lpuart.h"
#include "fsl_port.h"
#include "fsl_clock.h"
#include "fsl_reset.h"
#include "board.h"
#include "app.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define DEMO_LPUART2       LPUART2
#define RX_BUFFER_SIZE     256
#define TX_BUFFER_SIZE     128
#define TIMEOUT_LOOPS      2000000U

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void Init_LPUART2_Peripheral(void);
static status_t LPUART2_SendString(const char *str);
static bool LPUART2_ReceiveLine(char *buf, size_t max_len, uint32_t timeout_loops);
static int ParseJsonIntField(const char *json, const char *key, int defaultValue);

/*******************************************************************************
 * Variables
 ******************************************************************************/
static uint32_t g_cmdCount = 0U;

/*******************************************************************************
 * Code
 ******************************************************************************/

static void Init_LPUART2_Peripheral(void)
{
    /* Attach FRO_HF_DIV clock source to LPUART2 */
    CLOCK_AttachClk(kFRO_HF_DIV_to_LPUART2);
    CLOCK_SetClockDiv(kCLOCK_DivLPUART2, 1U);

    /* Enable PORT1 clock */
    CLOCK_EnableClock(kCLOCK_GatePORT1);

    /* Release peripheral resets for LPUART2 and PORT1 */
    RESET_ReleasePeripheralReset(kLPUART2_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kPORT1_RST_SHIFT_RSTn);

    /* PORT1_4 (pin 62) is configured as LPUART2_RXD */
    PORT_SetPinMux(PORT1, 4U, kPORT_MuxAlt3);
    PORT1->PCR[4] = ((PORT1->PCR[4] & (~(PORT_PCR_IBE_MASK))) | PORT_PCR_IBE(1U));

    /* PORT1_5 (pin 63) is configured as LPUART2_TXD */
    PORT_SetPinMux(PORT1, 5U, kPORT_MuxAlt3);
    PORT1->PCR[5] = ((PORT1->PCR[5] & (~(PORT_PCR_IBE_MASK))) | PORT_PCR_IBE(1U));

    /* Get dynamic functional clock frequency for LPUART2 */
    uint32_t clkFreq = CLOCK_GetLpuartClkFreq(2u);

    /* Initialize LPUART2 peripheral configuration */
    lpuart_config_t config;
    LPUART_GetDefaultConfig(&config);
    config.baudRate_Bps = 115200U;
    config.enableTx     = true;
    config.enableRx     = true;

    LPUART_Init(DEMO_LPUART2, &config, clkFreq);

    /* Clear any initial overrun or noise status flags */
    LPUART_ClearStatusFlags(DEMO_LPUART2, (uint32_t)kLPUART_RxOverrunFlag | (uint32_t)kLPUART_NoiseErrorFlag | (uint32_t)kLPUART_FramingErrorFlag);
}

static status_t LPUART2_SendString(const char *str)
{
    return LPUART_WriteBlocking(DEMO_LPUART2, (const uint8_t *)str, strlen(str));
}

static bool LPUART2_ReceiveLine(char *buf, size_t max_len, uint32_t timeout_loops)
{
    size_t idx = 0;
    uint32_t timeout = 0;

    memset(buf, 0, max_len);

    /* Clear error flags before polling */
    LPUART_ClearStatusFlags(DEMO_LPUART2, (uint32_t)kLPUART_RxOverrunFlag | (uint32_t)kLPUART_NoiseErrorFlag | (uint32_t)kLPUART_FramingErrorFlag);

    while (timeout < timeout_loops && idx < (max_len - 1))
    {
        uint32_t flags = LPUART_GetStatusFlags(DEMO_LPUART2);

        /* Clear overrun error if triggered during polling */
        if ((flags & (uint32_t)kLPUART_RxOverrunFlag) != 0U)
        {
            LPUART_ClearStatusFlags(DEMO_LPUART2, (uint32_t)kLPUART_RxOverrunFlag);
        }

        if ((flags & (uint32_t)kLPUART_RxDataRegFullFlag) != 0U)
        {
            uint8_t ch = LPUART_ReadByte(DEMO_LPUART2);
            buf[idx++] = (char)ch;
            if (ch == '\n')
            {
                buf[idx] = '\0';
                return true;
            }
            timeout = 0; /* Reset timeout on byte reception */
        }
        else
        {
            timeout++;
        }
    }

    if (idx > 0)
    {
        buf[idx] = '\0';
        return true;
    }

    return false;
}

static int ParseJsonIntField(const char *json, const char *key, int defaultValue)
{
    const char *pos = strstr(json, key);
    if (pos == NULL)
    {
        return defaultValue;
    }

    pos += strlen(key);

    /* Skip formatting symbols: quotes, colons, spaces */
    while (*pos == '"' || *pos == ':' || *pos == ' ' || *pos == '\t')
    {
        pos++;
    }

    int val = defaultValue;
    if (sscanf(pos, "%d", &val) == 1)
    {
        return val;
    }

    return defaultValue;
}

/*!
 * @brief Main function
 */
int main(void)
{
    char rxBuffer[RX_BUFFER_SIZE];
    char ackBuffer[TX_BUFFER_SIZE];

    /* Init board hardware (includes LPUART0 debug console). */
    BOARD_InitHardware();

    /* Init LPUART2 channel on PORT1_4 (RX) and PORT1_5 (TX) */
    Init_LPUART2_Peripheral();

    PRINTF("==============================================\r\n");
    PRINTF(" MCXA153 JSON Control Command Receiver        \r\n");
    PRINTF(" MCUX SDK version: %s\r\n", MCUXSDK_VERSION_FULL_STR);
    PRINTF(" Channel: LPUART2 (TX Pin 63 / RX Pin 62)      \r\n");
    PRINTF(" Baud Rate: 115200 8N1                         \r\n");
    PRINTF("==============================================\r\n");

    while (1)
    {
        PRINTF("\r\nWaiting for JSON command from ESP32-S3...\r\n");

        bool received = LPUART2_ReceiveLine(rxBuffer, sizeof(rxBuffer), TIMEOUT_LOOPS);
        if (received && strlen(rxBuffer) > 0)
        {
            g_cmdCount++;
            PRINTF("[RX JSON Packet #%u]: %s", (unsigned int)g_cmdCount, rxBuffer);

            /* Parse TT Motor and Servo control parameters */
            int leftSpeed  = ParseJsonIntField(rxBuffer, "left_speed", 0);
            int rightSpeed = ParseJsonIntField(rxBuffer, "right_speed", 0);
            int servoAngle = ParseJsonIntField(rxBuffer, "servo_angle", 0);
            int seqNum     = ParseJsonIntField(rxBuffer, "seq", (int)g_cmdCount);

            PRINTF("[PARSED PARAMETERS]: Seq=%d | Left Motor Speed=%d | Right Motor Speed=%d | Servo Angle=%d deg\r\n",
                   seqNum, leftSpeed, rightSpeed, servoAngle);

            /* Construct JSON ACK confirmation payload */
            snprintf(ackBuffer, sizeof(ackBuffer), "{\"status\":\"ACK\",\"seq\":%d}\n", seqNum);

            /* Transmit ACK confirmation over LPUART2 */
            status_t status = LPUART2_SendString(ackBuffer);
            if (status == kStatus_Success)
            {
                PRINTF("[TX ACK Response]: %s", ackBuffer);
                PRINTF("[STATUS]: PASS - ACK sent successfully to ESP32-S3\r\n");
            }
            else
            {
                PRINTF("[STATUS]: FAIL - Failed to transmit ACK over LPUART2 (status=%d)\r\n", (int)status);
            }
        }
        else
        {
            PRINTF("No command received (timeout). Waiting for next packet...\r\n");
        }
    }
}
