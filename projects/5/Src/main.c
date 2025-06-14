#include "stm32f10x.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/*
 * 腳位設定
 * - 超音波感測器 1（停車場入口）
 *   - PA1: ECHO (Input, EXTI1)
 *   - PA2: TRIG (Output)
 * - 超音波感測器 2（停車場出口）
 *   - PA3: ECHO (Input, EXTI3)
 *   - PA4: TRIG (Output)
 *
 * - 伺服馬達 (PWM)
 *   - PA8: 入口 (TIM1_CH1)
 *   - PA0: 出口 (TIM2_CH1)
 *
 * - 7 段顯示器
 *   - PB0-PB6: Segments a, b, c, d, e, f, g
 *   - PB8 Digit1
 *   - PB9 Digit2
 *
 * - Bluetooth
 *   - PA9:  USART1_TX
 *   - PA10: USART1_RX
 */

// Constants
#define kDistanceThreshold 1
#define kServoOpen 1500
#define kServoClosed 2000

const uint32_t kDisplayCode[] =
{ 0x3F, 0x6, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x27, 0x7F, 0x6F };

// Global State Variables
volatile uint32_t g_ms_ticks = 0;        // SysTick 計時器
volatile int g_parking_spaces = 20;      // 還有多少車位
volatile float g_entry_distance = 99.0f; // 入口測到的物體距離
volatile float g_exit_distance = 99.0f;  // 出口測到的物體距離

// Echo Measurement Variables
volatile uint32_t g_entry_echo_start = 0;
volatile uint32_t g_exit_echo_start = 0;
volatile int g_entry_echo_ongoing = 0;
volatile uint32_t g_entry_echo_start_ms = 0;
volatile int g_exit_echo_ongoing = 0;
volatile uint32_t g_exit_echo_start_ms = 0;

// Blinking state for full parking 車位滿時要閃爍
volatile int g_blink_state = 0;

volatile bool g_request_debug_print = false;

// Function Prototypes
void GPIO_config(void);
void USART1_config(void);
void TIM1_PWM_config(void);
void TIM2_PWM_config(void);
void SysTick_config(void);
void EXTI_config(void);

void delay_ms(uint16_t t);
void delay_us(uint16_t t);
void usart1_send_str(char *str);
void update_bluetooth_data(void);
void update_display_number(void);

// States
typedef enum
{
    STATE_IDLE, STATE_ENTRY_IN_PROGRESS, STATE_EXIT_IN_PROGRESS
} SystemState;

volatile SystemState g_system_state = STATE_IDLE;
volatile uint32_t g_gate_action_time = 0; // 記錄閘門開始動作的時間
volatile float g_last_triggered_distance = 0.0f;

int main()
{
    GPIO_config();
    USART1_config();
    TIM1_PWM_config();
    TIM2_PWM_config();
    SysTick_config();
    EXTI_config();

    TIM1->CCR1 = kServoClosed; // 關閉入口閘門
    TIM2->CCR1 = kServoClosed; // 關閉出口閘門

    update_bluetooth_data();
    update_display_number();

    while (true)
    {
        if (g_request_debug_print)
        {
            g_request_debug_print = false;
            char buffer[128]; // 準備一個足夠大的緩衝區
            // 將所有你想觀察的變數格式化成一個字串
            sprintf(buffer,
                    "ms:%lu, TrigDist:%.2f, Spaces:%d, State:%d\r\n",
                    g_ms_ticks, g_last_triggered_distance,
                    g_parking_spaces, g_system_state);

            // 透過 USART 送出字串
            usart1_send_str(buffer);
        }
        // State: IDLE（只有在 IDLE 時才偵測新的車輛進出）
        if (g_system_state == STATE_IDLE)
        {
            // 檢測入口車輛
            if (g_entry_distance < kDistanceThreshold && g_parking_spaces > 0)
            {
                g_last_triggered_distance = g_entry_distance;
                g_system_state = STATE_ENTRY_IN_PROGRESS;
                g_gate_action_time = g_ms_ticks; // 記錄目前的時間
                g_entry_distance = 99.0f; // 清除偵測到的距離，以免重複觸發

                TIM1->CCR1 = kServoOpen; // 打開入口閘門
            }
            // 檢測出口車輛
            else if (g_exit_distance < kDistanceThreshold)
            {
                g_last_triggered_distance = g_entry_distance;
                g_system_state = STATE_EXIT_IN_PROGRESS;
                g_gate_action_time = g_ms_ticks; // 記錄目前的時間
                g_exit_distance = 99.0f; // 清除距離

                TIM2->CCR1 = kServoOpen; // 打開出口閘門
            }
        }

        // State: 處理進入停車場的車輛
        if (g_system_state == STATE_ENTRY_IN_PROGRESS)
        {
            if (g_ms_ticks - g_gate_action_time > 1000)
            {
                TIM1->CCR1 = kServoClosed; // 關閉閘門

                if (g_parking_spaces > 0)
                    g_parking_spaces--;
                update_bluetooth_data();
                update_display_number();
                g_system_state = STATE_IDLE; // 回到 IDLE 狀態
            }
        }

        // State: 處理離開停車場的車輛
        if (g_system_state == STATE_EXIT_IN_PROGRESS)
        {
            if (g_ms_ticks - g_gate_action_time > 1000)
            {
                TIM2->CCR1 = kServoClosed; // 關閉閘門

                if (g_parking_spaces < 20)
                    g_parking_spaces++;
                update_bluetooth_data();
                update_display_number();

                g_system_state = STATE_IDLE;
            }
        }
    }
}

void GPIO_config(void)
{
    // 啟用 GPIO 和 AFIO 的 clock
    RCC->APB2ENR |=
    RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;

    // Servo
    GPIOA->CRL &= 0xFFFFFFF0;  // 清除 PA0 的設定
    GPIOA->CRH &= 0xFFFFFFF0;  // 清除 PA8 的設定
    GPIOA->CRL |= (0xB << 0);  // PA0 - 入口的 Servo 用 TIM2_CH1
    GPIOA->CRH |= (0xB << 0);  // PA8 - 出口的 Servo 用 TIM1_CH1

    // 超音波感測器
    GPIOA->CRL &= 0xFFF0000F;  // 清除 PA1, 2, 3, 4 的設定
    GPIOA->CRL |= (0x8 << 4);  // PA1 - 入口感測器 Echo，設定成 Input with pull-down
    GPIOA->CRL |= (0x8 << 12); // PA3 - 出口感測器 Echo，設定成 Input with pull-down
    GPIOA->ODR &= ~((1 << 1) | (1 << 3)); // 設定下拉電阻

    GPIOA->CRL |= (0x3 << 8);  // PA2 - 入口感測器 Trig，設定成 Output
    GPIOA->CRL |= (0x3 << 16); // PA4 - 出口感測器 Trig，設定成 Output

    // 七段顯示器 (GPIOB)
    GPIOB->CRL = 0x33333333; // PB0 to PB7 outputs
    GPIOB->CRH = 0x33333333; // PB8 to PB15 outputs

    // USART1
    GPIOA->CRH &= ~(0xFF << 4); // 清除 PA9, PA10 的設定
    GPIOA->CRH |= (0x0B << 4); // PA9: 50MHz, AF output push-pull
    GPIOA->CRH |= (0x04 << 8); // PA10: Input with pull-up / pull-down
}

void TIM1_PWM_config(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN; // 開啟 TIM1 clock

    // Channel 1 (PA8, 入口)
    TIM1->CCER |= 0x1;
    TIM1->CCMR1 |= 0x60;

    TIM1->PSC = 72 - 1;
    TIM1->ARR = 20000 - 1;

    TIM1->CCR1 = kServoOpen; // 調整到預設位置
    TIM1->CR1 = 1;
}

void TIM2_PWM_config(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN; // 開啟 TIM2 clock

    // Channel 1 (PA0, 出口)
    TIM2->CCER |= 0x1;
    TIM2->CCMR1 |= 0x60;

    TIM2->PSC = 72 - 1;
    TIM2->ARR = 20000 - 1;

    TIM2->CCR1 = kServoOpen; // 調整到預設位置
    TIM2->CR1 = 1;
}

void USART1_config(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    USART1->BRR = (468 << 4) | 12;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void SysTick_config(void)
{
    if (SysTick_Config(SystemCoreClock / 1000))
    {
        while (1)
            ;
    }
}

void SysTick_Handler(void)
{
    g_ms_ticks++; // 全域計時

    static uint32_t trigger_counter = 0; // 累加時間用
    static bool entry_exit_toggle = false;    // 交替出入口

    if (g_ms_ticks % 500 == 0)
    {
        g_request_debug_print = true;
    }

    if (++trigger_counter >= 2500) // 當計時器計到 2.5 秒
    {
        trigger_counter = 0; // 歸零

        // 使用 toggle 變數來決定這次要觸發入口還是出口的超音波感測器
        if (entry_exit_toggle == false) // 入口
        {
            GPIOA->BSRR = GPIO_BSRR_BS2; // PA2 High
            delay_us(10);
            GPIOA->BSRR = GPIO_BSRR_BR2; // PA2 Low
        }
        else // 出口
        {
            GPIOA->BSRR = GPIO_BSRR_BS4; // PA4 High
            delay_us(10);
            GPIOA->BSRR = GPIO_BSRR_BR4; // PA4 Low
        }
        entry_exit_toggle = !entry_exit_toggle;
    }

}

void EXTI_config(void)
{
    // 啟用 AFIO clock
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;

    // 連接 EXTI Line 1 到 PA1, Line 3 到 PA3
    AFIO->EXTICR[0] |= AFIO_EXTICR1_EXTI1_PA | AFIO_EXTICR1_EXTI3_PA;

    // 設定 EXTI line 1 & 3
    EXTI->IMR |= EXTI_IMR_MR1 | EXTI_IMR_MR3;    // 解除中斷遮罩
    EXTI->RTSR |= EXTI_RTSR_TR1 | EXTI_RTSR_TR3; // 啟用上升緣觸發
    EXTI->FTSR |= EXTI_FTSR_TR1 | EXTI_FTSR_TR3; // 啟用下降緣觸發

    // 在 NVIC 中啟用中斷
    NVIC_EnableIRQ(EXTI1_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);
}

void EXTI1_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR1)
    {
        if (GPIOA->IDR & GPIO_IDR_IDR1)
        { // 上升緣
            g_entry_echo_start_ms = g_ms_ticks;
            g_entry_echo_start = SysTick->VAL;
            g_entry_echo_ongoing = 1;
        }
        else
        { // 下降緣
            if (g_entry_echo_ongoing)
            {
                // 使用 int64_t 計算，確保不會溢位
                int64_t start_ticks = (int64_t) g_entry_echo_start_ms * 72000
                        + (72000 - g_entry_echo_start);
                int64_t end_ticks = (int64_t) g_ms_ticks * 72000
                        + (72000 - SysTick->VAL);

                int64_t duration_ticks = end_ticks - start_ticks;

                // 安全性檢查，避免因意外得到負值
                if (duration_ticks < 0)
                {
                    duration_ticks = 0;
                }

                g_entry_distance = ((float) duration_ticks / 72000000.0f)
                        * 343.0f / 2.0f;
                g_entry_echo_ongoing = 0;
            }
        }
        EXTI->PR = EXTI_PR_PR1;
    }
}

// 出口 Echo (PA3) 的中斷處理函式
void EXTI3_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR3)
    {
        if (GPIOA->IDR & GPIO_IDR_IDR3)
        { // 上升緣
            g_exit_echo_start_ms = g_ms_ticks;
            g_exit_echo_start = SysTick->VAL;
            g_exit_echo_ongoing = 1;
        }
        else
        { // 下降緣
            if (g_exit_echo_ongoing)
            {
                // 使用 int64_t 做計算，確保不會溢位
                int64_t start_ticks = (int64_t) g_exit_echo_start_ms * 72000
                        + (72000 - g_exit_echo_start);
                int64_t end_ticks = (int64_t) g_ms_ticks * 72000
                        + (72000 - SysTick->VAL);

                int64_t duration_ticks = end_ticks - start_ticks;

                // 安全性檢查，避免因意外得到負值
                if (duration_ticks < 0)
                {
                    duration_ticks = 0;
                }

                g_exit_distance = ((float) duration_ticks / 72000000.0f)
                        * 343.0f / 2.0f;
                g_exit_echo_ongoing = 0;
            }
        }
        EXTI->PR = EXTI_PR_PR3;
    }
}

void update_bluetooth_data(void)
{
    int cars_parked = 20 - g_parking_spaces;
    char buffer[20];
    sprintf(buffer, "%02d\r\n", cars_parked);
    usart1_send_str(buffer);
}

void update_display_number(void)
{
    uint32_t low = kDisplayCode[g_parking_spaces % 10];
    uint32_t high = kDisplayCode[g_parking_spaces / 10];
    GPIOB->ODR = (high << 8) | low;
}

void usart1_send_str(char *str)
{
    while (*str)
    {
        while (!(USART1->SR & USART_SR_TC))
            ;
        USART1->DR = *str;
        str++;
    }
}

void delay_ms(uint16_t t)
{
    volatile unsigned long l;
    for (uint16_t i = 0; i < t; i++)
    {
        for (l = 0; l < 9000; l++)
            ;
    }
}

void delay_us(uint16_t t)
{
    volatile unsigned long l;
    for (uint16_t i = 0; i < t; i++)
    {
        for (l = 0; l < 7; l++)
            ;
    }
}

