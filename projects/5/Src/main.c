#include "stm32f10x.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/*
 * - 入口超音波感測器 (Entry Sensor):
 * - Trig (觸發腳): PC13
 * - Echo (回波腳): PA2 (對應 EXTI2 中斷)
 *
 * - 出口超音波感測器 (Exit Sensor):
 * - Trig (觸發腳): PC14
 * - Echo (回波腳): PA1 (對應 EXTI1 中斷)
 *
 * - 入口閘門伺服馬達 (Entry Gate Servo):
 * - PWM 信號線: PA8 (使用 TIM1 Channel 1)
 *
 * - 出口閘門伺服馬達 (Exit Gate Servo):
 * - PWM 信號線: PA0 (使用 TIM2 Channel 1)
 *
 * - 連接至 USART1:
 * - TX (傳送): PA9
 * - RX (接收): PA10
 *
 * - 七段顯示器：
 * - 個位數 (Units Digit):
 * - a-g 段: PB0 - PB6
 *
 * - 十位數 (Tens Digit):
 * - a-g 段: PB8 - PB14
 */

// Constants
#define kTriggerGap 5 * 1000 * 1000 // 觸發間隔 (5 秒)
#define kServoDelay 450            // Servo 閘門開啟後延遲多久關閉
#define kServoOpen 1500
#define kServoEntryClosed 2500
#define kServoExitClosed 500
#define kTxBufferSize 128           // USART 發送緩衝區 Buffer 大小

#define kSysClockFreq 27000000
#define kTim1ClockFreq 144000000
#define kTim2ClockFreq 72000000
#define kUsart1ClockFreq 72000000

// Global variables
// --- 超音波感測器
volatile uint32_t g_entry_echo_duration_us = 0;
volatile uint32_t g_exit_echo_duration_us = 0;
static volatile uint32_t g_entry_echo_start_us = 0;
static volatile uint32_t g_exit_echo_start_us = 0;
volatile bool g_is_entry_detected = false;
volatile bool g_is_exit_detected = false;

// --- 計時與計數
volatile uint32_t g_us_ticks = 0; // us 計時器
volatile int g_remaining_spaces = 20;

// USART
char g_tx_buffer[kTxBufferSize];
volatile uint16_t g_tx_head = 0;
volatile uint16_t g_tx_tail = 0;

// 函式宣告
void delay_us(uint32_t us); // 延遲 us
void delay_ms(uint32_t ms); // 延遲 ms
void usart1_send_str(char *str);
void update_display(int count);

int main(void)
{
    // --- Clock 初始化 ---
    // 啟用外設時脈
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN
            | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_TIM1EN | RCC_APB2ENR_USART1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    // --- GPIO 初始化 ---
    GPIOB->CRL = 0x33333333;
    GPIOB->CRH = 0x33333333;
    GPIOC->CRH = (GPIOC->CRH & ~0x0FF00000) | 0x03300000;
    // 合併設定 PA0, PA1, PA2
    GPIOA->CRL = (GPIOA->CRL & ~0x00000FFF) | (0x8 << 8) | (0x8 << 4)
            | (0xB << 0);
    GPIOA->ODR &= ~((1 << 1) | (1 << 2));
    // 合併設定 PA8, PA9, PA10
    GPIOA->CRH = (GPIOA->CRH & ~0x00000FFF) | (0x4 << 8) | (0xB << 4)
            | (0xB << 0);

    // --- SysTick 初始化 (10µs tick, 每 tick 將全域的 g_us_ticks += 10) ---
    SysTick->LOAD = (kSysClockFreq / 100000) - 1;
    SysTick->VAL = 0;
    // SysTick 啟用計數器、啟用中斷
    SysTick->CTRL = SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk;

    // --- TIM1 & TIM2 PWM 初始化 ---
    TIM1->PSC = (kTim1ClockFreq / 1000000) - 1;
    TIM1->ARR = 20000 - 1;
    TIM1->CCR1 = kServoEntryClosed;

    TIM1->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE;
    TIM1->CCER = TIM_CCER_CC1E;
    TIM1->BDTR = TIM_BDTR_MOE;
    TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;

    TIM2->PSC = (kTim2ClockFreq / 1000000) - 1;
    TIM2->ARR = 20000 - 1;
    TIM2->CCR1 = kServoExitClosed;

    TIM2->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE;
    TIM2->CCER = TIM_CCER_CC1E;
    TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;

    // --- USART1 初始化 ---
    USART1->BRR = kUsart1ClockFreq / 9600;
    // 啟用 USART、傳送器、接收器及接收中斷
    USART1->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_TE | USART_CR1_RXNEIE;

    // --- EXTI & NVIC 初始化 ---
    AFIO->EXTICR[0] = (AFIO->EXTICR[0] & ~0x0FF0) | (0x0 << 8) | (0x0 << 4);
    // 設定中斷遮罩
    EXTI->IMR |= EXTI_IMR_MR1 | EXTI_IMR_MR2;
    // 設定上升沿觸發
    EXTI->RTSR |= EXTI_RTSR_TR1 | EXTI_RTSR_TR2;
    // 設定下降沿觸發
    EXTI->FTSR |= EXTI_FTSR_TR1 | EXTI_FTSR_TR2;
    // 使用 IRQn 編號來啟用中斷
    NVIC->ISER[0] |= (1U << EXTI1_IRQn) | (1U << EXTI2_IRQn);
    NVIC->ISER[1] |= (1U << (USART1_IRQn - 32));

    // --- 主迴圈 ---
    uint32_t last_bt_send_time_us = 0;
    uint32_t last_entry_trig_time_us = 0;
    uint32_t last_exit_trig_time_us = 0;
    bool exit_trig_started = false; // 用於標記出口觸發是否已開始

    update_display(g_remaining_spaces);

    while (1)
    {
        float distance_m;
        char buffer[30];

        // --- 主迴圈中的週期性 Trig ---
        if (g_us_ticks - last_entry_trig_time_us >= kTriggerGap)
        {
            // 5 秒
            last_entry_trig_time_us = g_us_ticks;
            GPIOC->BSRR = GPIO_BSRR_BS13;
            delay_us(10);
            GPIOC->BRR = GPIO_BRR_BR13;
        }

        // 延遲 2.5 秒後才開始出口的 Trig
        if (!exit_trig_started && g_us_ticks >= kTriggerGap / 2)
        {
            exit_trig_started = true;
            last_exit_trig_time_us = g_us_ticks;
        }

        if (exit_trig_started
                && (g_us_ticks - last_exit_trig_time_us >= kTriggerGap))
        {
            // 5 秒
            last_exit_trig_time_us = g_us_ticks;
            GPIOC->BSRR = GPIO_BSRR_BS14;
            delay_us(10);
            GPIOC->BRR = GPIO_BRR_BR14;
        }

        // --- 處理感測器 detect ---
        if (g_is_entry_detected)
        {
            g_is_entry_detected = false;

            distance_m = (g_entry_echo_duration_us * 343.0 / 2.0) / 1000000.0;
            if (distance_m < 1.0 && g_remaining_spaces > 0)
            {
                g_remaining_spaces--;
                TIM1->CCR1 = kServoOpen;
                delay_ms(kServoDelay);
                TIM1->CCR1 = kServoEntryClosed;
            }
        }

        if (g_is_exit_detected)
        {
            g_is_exit_detected = false;

            distance_m = (g_exit_echo_duration_us * 343.0 / 2.0) / 1000000.0;

            if (distance_m < 1.0 && g_remaining_spaces < 20)
            {
                g_remaining_spaces++;
                TIM2->CCR1 = kServoOpen;
                delay_ms(kServoDelay);
                TIM2->CCR1 = kServoExitClosed;
            }
        }

        if (g_remaining_spaces == 0)
        {
            if ((g_us_ticks / 500000) % 2)
            {
                GPIOB->ODR = 0x0000;
            }
            else
            {
                update_display(0);
            }
        }
        else
        {
            update_display(g_remaining_spaces);
        }

        if (g_us_ticks - last_bt_send_time_us >= 500000)
        {
            last_bt_send_time_us = g_us_ticks;
            sprintf(buffer, "Current Cars: %02d\r\n", 20 - g_remaining_spaces);
            usart1_send_str(buffer);
        }
    }
}

void delay_us(uint32_t us)
{
    uint32_t start = g_us_ticks;
    while ((g_us_ticks - start) < us)
        ;
}

void delay_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++)
    {
        delay_us(1000);
    }
}

// ISRs (中斷)
// SysTick 中斷：只負責累加 g_us_ticks
void SysTick_Handler(void)
{
    g_us_ticks += 10; // 每 10us 一次 += 10
}

void USART1_IRQHandler(void)
{
    // 檢查是否為 TXE (發送緩存區空) 中斷
    if ((USART1->SR & USART_SR_TXE) != 0)
    {
        if (g_tx_head != g_tx_tail)
        {
            // 如果緩衝區還有資料，發送下一個字元
            USART1->DR = g_tx_buffer[g_tx_tail];
            g_tx_tail = (g_tx_tail + 1) % kTxBufferSize;
        }
        else
        {
            // 如果緩衝區已空，關閉 TXE 中斷
            USART1->CR1 &= ~USART_CR1_TXEIE;
        }
    }
}

// EXTI 中斷
void EXTI1_IRQHandler(void)
{ // 出口 Echo (PA1)
    if ((EXTI->PR & EXTI_PR_PR1) != 0)
    {
        if ((GPIOA->IDR & GPIO_IDR_IDR1) != 0)
        {
            g_exit_echo_start_us = g_us_ticks;
        }
        else
        {
            g_exit_echo_duration_us = g_us_ticks - g_exit_echo_start_us;
            g_is_exit_detected = true;
        }
        EXTI->PR = EXTI_PR_PR1; // 清除中斷
    }
}

void EXTI2_IRQHandler(void)
{ // 入口 Echo (PA2)
    if ((EXTI->PR & EXTI_PR_PR2) != 0)
    {
        if ((GPIOA->IDR & GPIO_IDR_IDR2) != 0)
        {
            g_entry_echo_start_us = g_us_ticks;
        }
        else
        {
            g_entry_echo_duration_us = g_us_ticks - g_entry_echo_start_us;
            g_is_entry_detected = true;
        }
        EXTI->PR = EXTI_PR_PR2; // 清除中斷
    }
}

void usart1_send_str(char *str)
{
    while (*str)
    {
        // 將字元放入緩衝區
        g_tx_buffer[g_tx_head] = *str++;
        g_tx_head = (g_tx_head + 1) % kTxBufferSize;
    }
    // 啟用 TXE 中斷，開始發送過程
    USART1->CR1 |= USART_CR1_TXEIE;
}

void update_display(int count)
{
    uint16_t arr[10] =
    { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x27, 0x7F, 0x6F };
    if (count >= 0 && count <= 99)
    {
        GPIOB->ODR = (arr[count / 10] << 8) | arr[count % 10];
    }
}
