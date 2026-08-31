/*
 * Copyright 2019 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"
#include "app.h"
#include "frdmmcxa153/board/pin_mux.h"
#include "frdmmcxa153/board/peripherals.h"
#include "fsl_gpio.h"
#include "fsl_port.h"
#include "fsl_clock.h"
#include "fsl_reset.h"
#include "fsl_pwm.h"
#include "fsl_debug_console.h"
#include <stdbool.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define MOTOR_TEST_DUTY_CYCLE_PERCENT 35U
#define MOTOR_PWM_FREQ_HZ             10000U

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void MotorDual_SetControlPwm(bool standby, uint8_t duty_a, uint8_t ain1, uint8_t ain2, uint8_t duty_b, uint8_t bin1, uint8_t bin2);
static void App_InitMotors(void);

/*******************************************************************************
 * Variables
 ******************************************************************************/
static volatile uint32_t g_seconds = 0U;
static volatile bool g_motorInitialized = false;

/*******************************************************************************
 * Code
 ******************************************************************************/
static void App_InitMotors(void)
{
    /* Enable peripheral clocks for GPIO, PORT, and FLEXPWM0 */
    CLOCK_EnableClock(kCLOCK_GatePORT2);
    CLOCK_EnableClock(kCLOCK_GatePORT3);
    CLOCK_EnableClock(kCLOCK_GateGPIO2);
    CLOCK_EnableClock(kCLOCK_GateGPIO3);
    CLOCK_EnableClock(kCLOCK_GateFLEXPWM0);

    /* Release peripheral resets */
    RESET_ReleasePeripheralReset(kGPIO2_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kPORT2_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kGPIO3_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kPORT3_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kFLEXPWM0_RST_SHIFT_RSTn);

    /* Enable PORT Input Buffer (IBE), High Drive Strength (DSE), and Pin Muxing for Motor Channel A pins */
    PORT2->PCR[5] = (PORT2->PCR[5] & ~(PORT_PCR_MUX_MASK | PORT_PCR_PE_MASK)) | PORT_PCR_MUX(kPORT_MuxAlt0) | PORT_PCR_IBE_MASK | PORT_PCR_DSE_MASK; /* AIN2 */
    PORT2->PCR[7] = (PORT2->PCR[7] & ~(PORT_PCR_MUX_MASK | PORT_PCR_PE_MASK)) | PORT_PCR_MUX(kPORT_MuxAlt0) | PORT_PCR_IBE_MASK | PORT_PCR_DSE_MASK; /* STBY_DRIVER */
    PORT3->PCR[0] = (PORT3->PCR[0] & ~(PORT_PCR_MUX_MASK | PORT_PCR_PE_MASK)) | PORT_PCR_MUX(kPORT_MuxAlt0) | PORT_PCR_IBE_MASK | PORT_PCR_DSE_MASK; /* AIN1 (High Drive for LED pin) */

    /* Configure PWMA pin on PORT3 with kPORT_MuxAlt5 (FlexPWMA0) and DSE/IBE enabled */
    PORT3->PCR[6] = (PORT3->PCR[6] & ~(PORT_PCR_MUX_MASK | PORT_PCR_PE_MASK)) | PORT_PCR_MUX(kPORT_MuxAlt5) | PORT_PCR_IBE_MASK | PORT_PCR_DSE_MASK; /* FlexPWMA0 (P3_6) */

    /* Enable PORT Input Buffer (IBE), High Drive Strength (DSE), and Pin Muxing for Motor Channel B pins */
    PORT3->PCR[12] = (PORT3->PCR[12] & ~(PORT_PCR_MUX_MASK | PORT_PCR_PE_MASK)) | PORT_PCR_MUX(kPORT_MuxAlt0) | PORT_PCR_IBE_MASK | PORT_PCR_DSE_MASK; /* BIN1 (High Drive for LED pin) */
    PORT3->PCR[13] = (PORT3->PCR[13] & ~(PORT_PCR_MUX_MASK | PORT_PCR_PE_MASK)) | PORT_PCR_MUX(kPORT_MuxAlt0) | PORT_PCR_IBE_MASK | PORT_PCR_DSE_MASK; /* BIN2 */

    /* Configure PWMB pin on PORT3 with kPORT_MuxAlt5 (FlexPWMA1) and DSE/IBE enabled */
    PORT3->PCR[8] = (PORT3->PCR[8] & ~(PORT_PCR_MUX_MASK | PORT_PCR_PE_MASK)) | PORT_PCR_MUX(kPORT_MuxAlt5) | PORT_PCR_IBE_MASK | PORT_PCR_DSE_MASK; /* FlexPWMA1 (P3_8) */

    /* Configure Motor direction and standby pins as digital GPIO outputs */
    gpio_pin_config_t out_config_low = {
        .pinDirection = kGPIO_DigitalOutput,
        .outputLogic = 0U
    };

    gpio_pin_config_t out_config_high = {
        .pinDirection = kGPIO_DigitalOutput,
        .outputLogic = 1U
    };

    GPIO_PinInit(BOARD_INITPINS_STBY_DRIVER_GPIO, BOARD_INITPINS_STBY_DRIVER_GPIO_PIN, &out_config_high);
    GPIO_PinInit(BOARD_INITPINS_AIN1_GPIO, BOARD_INITPINS_AIN1_GPIO_PIN, &out_config_high);
    GPIO_PinInit(BOARD_INITPINS_AIN2_GPIO, BOARD_INITPINS_AIN2_GPIO_PIN, &out_config_low);
    GPIO_PinInit(BOARD_INITPINS_BIN1_GPIO, BOARD_INITPINS_BIN1_GPIO_PIN, &out_config_high);
    GPIO_PinInit(BOARD_INITPINS_BIN2_GPIO, BOARD_INITPINS_BIN2_GPIO_PIN, &out_config_low);

    /* Local PWM configuration structure */
    pwm_config_t pwmConfig;
    PWM_GetDefaultConfig(&pwmConfig);
    pwmConfig.reloadLogic = kPWM_ReloadImmediate;
    pwmConfig.pairOperation = kPWM_Independent;

    /* Signal parameters with kPWM_LowTrue polarity to fix duty cycle inversion */
    pwm_signal_param_t pwmSignalSM0 = {
        .pwmChannel = kPWM_PwmA,
        .dutyCyclePercent = MOTOR_TEST_DUTY_CYCLE_PERCENT,
        .level = kPWM_LowTrue,
        .faultState = kPWM_PwmFaultState0,
        .pwmchannelenable = true,
        .deadtimeValue = 0U
    };

    pwm_signal_param_t pwmSignalSM1 = {
        .pwmChannel = kPWM_PwmA,
        .dutyCyclePercent = MOTOR_TEST_DUTY_CYCLE_PERCENT,
        .level = kPWM_LowTrue,
        .faultState = kPWM_PwmFaultState0,
        .pwmchannelenable = true,
        .deadtimeValue = 0U
    };

    /* Initialize PWM submodules SM0 (PWMA) and SM1 (PWMB) */
    PWM_Init(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, &pwmConfig);
    PWM_Init(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, &pwmConfig);

    /* Clear any latched fault flags in FLEXPWM0 hardware */
    FLEXPWM0_PERIPHERAL->FSTS = 0x000FU;

    /* Disable fault override logic on channel outputs */
    PWM_SetupFaultDisableMap(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, FLEXPWM0_SM0_A, kPWM_faultchannel_0, 0U);
    PWM_SetupFaultDisableMap(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, FLEXPWM0_SM1_A, kPWM_faultchannel_0, 0U);

    /* Setup PWM outputs for SM0 and SM1 at 10 kHz Edge-Aligned */
    PWM_SetupPwm(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, &pwmSignalSM0, 1U, kPWM_EdgeAligned, MOTOR_PWM_FREQ_HZ, 12000000U);
    PWM_SetupPwm(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, &pwmSignalSM1, 1U, kPWM_EdgeAligned, MOTOR_PWM_FREQ_HZ, 12000000U);

    /* Unmask output channels and route PWM generator signals to output pins */
    PWM_SetChannelOutput(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, FLEXPWM0_SM0_A, kPWM_NormalState);
    PWM_SetChannelOutput(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, FLEXPWM0_SM1_A, kPWM_NormalState);
    PWM_SetupForceSignal(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, FLEXPWM0_SM0_A, kPWM_UsePwm);
    PWM_SetupForceSignal(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, FLEXPWM0_SM1_A, kPWM_UsePwm);

    /* Update initial duty cycles */
    PWM_UpdatePwmDutycycle(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, FLEXPWM0_SM0_A, kPWM_EdgeAligned, MOTOR_TEST_DUTY_CYCLE_PERCENT);
    PWM_UpdatePwmDutycycle(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, FLEXPWM0_SM1_A, kPWM_EdgeAligned, MOTOR_TEST_DUTY_CYCLE_PERCENT);

    /* Load initial PWM parameters for submodules 0 and 1 */
    PWM_SetPwmLdok(FLEXPWM0_PERIPHERAL, (kPWM_Control_Module_0 | kPWM_Control_Module_1), true);

    /* Start PWM timers for submodules 0 and 1 */
    PWM_StartTimer(FLEXPWM0_PERIPHERAL, (kPWM_Control_Module_0 | kPWM_Control_Module_1));

    /* Mark initialization complete */
    g_motorInitialized = true;
}

static void MotorDual_SetControlPwm(bool standby, uint8_t duty_a, uint8_t ain1, uint8_t ain2, uint8_t duty_b, uint8_t bin1, uint8_t bin2)
{
    /* Set Standby driver pin (GPIO2 Pin 7) */
    GPIO_PinWrite(BOARD_INITPINS_STBY_DRIVER_GPIO, BOARD_INITPINS_STBY_DRIVER_GPIO_PIN, standby ? 1U : 0U);

    /* Set Motor A direction inputs (AIN1: GPIO3 Pin 0, AIN2: GPIO2 Pin 5) */
    GPIO_PinWrite(BOARD_INITPINS_AIN1_GPIO, BOARD_INITPINS_AIN1_GPIO_PIN, ain1 ? 1U : 0U);
    GPIO_PinWrite(BOARD_INITPINS_AIN2_GPIO, BOARD_INITPINS_AIN2_GPIO_PIN, ain2 ? 1U : 0U);

    /* Set Motor B direction inputs (BIN1: GPIO3 Pin 12, BIN2: GPIO3 Pin 13) */
    GPIO_PinWrite(BOARD_INITPINS_BIN1_GPIO, BOARD_INITPINS_BIN1_GPIO_PIN, bin1 ? 1U : 0U);
    GPIO_PinWrite(BOARD_INITPINS_BIN2_GPIO, BOARD_INITPINS_BIN2_GPIO_PIN, bin2 ? 1U : 0U);

    /* Clear any latched fault flags */
    FLEXPWM0_PERIPHERAL->FSTS = 0x000FU;

    /* Unmask output channels */
    PWM_SetChannelOutput(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, FLEXPWM0_SM0_A, kPWM_NormalState);
    PWM_SetChannelOutput(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, FLEXPWM0_SM1_A, kPWM_NormalState);

    /* Update PWM speed control duty cycle for FlexPWMA0 (SM0_A) and FlexPWMA1 (SM1_A) */
    PWM_UpdatePwmDutycycle(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM0, FLEXPWM0_SM0_A, kPWM_EdgeAligned, duty_a);
    PWM_UpdatePwmDutycycle(FLEXPWM0_PERIPHERAL, FLEXPWM0_SM1, FLEXPWM0_SM1_A, kPWM_EdgeAligned, duty_b);

    /* Load OK to activate new PWM duty cycle parameters synchronously */
    PWM_SetPwmLdok(FLEXPWM0_PERIPHERAL, (kPWM_Control_Module_0 | kPWM_Control_Module_1), true);
}

void SysTick_Handler(void)
{
    if (!g_motorInitialized)
    {
        return;
    }

    g_seconds++;

    /* Synchronized Dual Motor Drive Routine */
    if (g_seconds == 4U)
    {
        /* 4s: Pause both motors (0% PWM) */
        PRINTF("[4s] Dual Channels A & B: PAUSE (0%% FlexPWM Duty Cycle)\r\n");
        MotorDual_SetControlPwm(true, 0U, 0U, 0U, 0U, 0U, 0U);
    }
    else if (g_seconds == 5U)
    {
        /* 5s to 9s: Dual Reverse drive both motors in sync at 50% PWM speed */
        PRINTF("[5s] Dual Channels A & B: REVERSE (50%% FlexPWM Duty Cycle)\r\n");
        MotorDual_SetControlPwm(true, MOTOR_TEST_DUTY_CYCLE_PERCENT, 0U, 1U, MOTOR_TEST_DUTY_CYCLE_PERCENT, 0U, 1U);
    }
    else if (g_seconds >= 9U)
    {
        /* 9s+: Test complete - stop both motors and enter standby mode */
        PRINTF("[9s] TEST COMPLETE: Dual Channels A & B stopped, driver in STANDBY\r\n");
        MotorDual_SetControlPwm(false, 0U, 0U, 0U, 0U, 0U, 0U);
    }
}

/*!
 * @brief Main function
 */
int main(void)
{
    /* Board pin init */
    BOARD_InitHardware();

    /* Initialize Debug Console over LPUART0 (115200 baud) */
    BOARD_InitDebugConsole();

    PRINTF("\r\n=== TB6612FNG Dual-Channel Synchronized FlexPWM Drive ===\r\n");

    /* App motor hardware & FlexPWM init */
    App_InitMotors();

    /* Immediately drive both Motor A and Motor B forward in sync at 50% PWM speed */
    PRINTF("[0s] Dual Channels A & B: FORWARD (50%% FlexPWM Duty Cycle)\r\n");
    MotorDual_SetControlPwm(true, MOTOR_TEST_DUTY_CYCLE_PERCENT, 1U, 0U, MOTOR_TEST_DUTY_CYCLE_PERCENT, 1U, 0U);

    while (1)
    {
    }
}








