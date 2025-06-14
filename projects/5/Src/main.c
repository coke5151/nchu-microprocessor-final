#include "stm32f10x.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/*
 * 腳位設定
 * - 超音波感測器 1（停車場入口）
 *   - PA4: TRIG (Output)
 *   - PA1: ECHO (Input, EXTI1)
 * - 超音波感測器 2（停車場出口）
 *   - PA2: TRIG (Output)
 *   - PA3: ECHO (Input, EXTI3)
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
#define kDistanceThreshold 0.1f
#define kServoOpen 1500
#define kServoClosed 500

const uint32_t kDisplayCode[] =
{ 0x3F, 0x6, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x27, 0x7F, 0x6F };

// Global State Variables
volatile int g_parking_spaces = 20;      // 還有多少車位
volatile float g_entry_distance = 99.0f; // 入口測到的物體距離
volatile float g_exit_distance = 99.0f;  // 出口測到的物體距離

// Echo Measurement Variables
volatile uint32_t g_entry_echo_start = 0;
volatile uint32_t g_exit_echo_start = 0;
volatile int g_entry_echo_ongoing = 0;
volatile int g_exit_echo_ongoing = 0;

// Blinking state for full parking 車位滿時要閃爍
volatile int g_blink_state = 0;

// Function Prototypes
void GPIO_config(void);
void USART1_config(void);
void TIM1_PWM_config(void);
void TIM2_PWM_config(void);
void EXTI_config(void);
void SysTick_config(void);

void delay_ms(uint16_t t);
void delay_us(uint16_t t);
void usart1_send_str(char *str);
void update_bluetooth_data(void);
void update_display_number(void);

int main()
{
    GPIO_config();
    USART1_config();
    TIM1_PWM_config();
    TIM2_PWM_config();
    EXTI_config();
    SysTick_config();

    TIM1->CCR1 = kServoClosed; // 關閉入口閘門
    TIM2->CCR1 = kServoClosed; // 關閉出口閘門
    update_bluetooth_data();
    update_display_number();

    while (true)
    {
        // Entry
        if (g_entry_distance < kDistanceThreshold
                && g_parking_spaces > 0&& TIM1-> CCR1 == kServoClosed)
        {
            TIM1->CCR1 = kServoOpen;
            delay_ms(2000);
            if (g_parking_spaces > 0)
            {
                g_parking_spaces--;
            }
            update_bluetooth_data();
            update_display_number();
            TIM1->CCR1 = kServoClosed;
            delay_ms(500);
            g_entry_distance = 99.0f;
        }

        // Exit
        if (g_exit_distance < kDistanceThreshold && TIM2->CCR1 == kServoClosed)
        {
            TIM2->CCR1 = kServoOpen;
            delay_ms(2000);
            if (g_parking_spaces < 20)
            {
                g_parking_spaces++;
            }
            update_bluetooth_data();
            update_display_number();
            TIM2->CCR1 = kServoClosed;
            delay_ms(500);
            g_exit_distance = 99.0f;
        }
    }
}

void GPIO_config(void)
{
    RCC->APB2ENR |=
    RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;

    // GPIOA
    // PA0 (TIM2_CH1 for Exit Servo)
    GPIOA->CRL &= ~(0xF << 0);
    GPIOA->CRL |= (0xB << 0);

    // PA1, PA3 (Echo inputs)
    GPIOA->CRL &= ~((0xF << 4) | (0xF << 12));
    GPIOA->CRL |= (0x4 << 4) | (0x4 << 12);

    // PA2 (Exit Trig), PA4 (Entry Trig)
    GPIOA->CRL &= ~((0xF << 8) | (0xF << 16));
    GPIOA->CRL |= (0x3 << 8) | (0x3 << 16);

    // PA8 (TIM1_CH1 for Entry Servo)
    GPIOA->CRH &= ~(0xF << 0);
    GPIOA->CRH |= (0xB << 0);

    // PA9 (USART1_TX) / PA10 (USART1_RX)
    GPIOA->CRH &= ~((0xF << 4) | (0xF << 8));
    GPIOA->CRH |= (0xB << 4) | (0x4 << 8);

    // GPIOB (7 段顯示器)
    GPIOB->CRL = 0x33333333; // PB0 to PB7 outputs
    GPIOB->CRH = 0x33333333; // PB8 to PB15 outputs
}

void TIM1_PWM_config(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;

    TIM1->PSC = 72 - 1;
    TIM1->ARR = 20000 - 1;

    // Channel 1 (PA8, Entry)
    TIM1->CCMR1 |= TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2;
    TIM1->CCMR1 |= TIM_CCMR1_OC1PE;
    TIM1->CCER |= TIM_CCER_CC1E;

    TIM1->BDTR |= TIM_BDTR_MOE;
    TIM1->CR1 |= TIM_CR1_CEN;
}

void TIM2_PWM_config(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    TIM2->PSC = 72 - 1;
    TIM2->ARR = 20000 - 1;

    // Channel 1 (PA0, Exit)
    TIM2->CCMR1 |= TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2;
    TIM2->CCMR1 |= TIM_CCMR1_OC1PE;
    TIM2->CCER |= TIM_CCER_CC1E;

    TIM2->CR1 |= TIM_CR1_CEN;
}

void USART1_config(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    USART1->BRR = (468 << 4) | 12;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void EXTI_config(void)
{
    AFIO->EXTICR[0] |= AFIO_EXTICR1_EXTI1_PA;
    AFIO->EXTICR[0] |= AFIO_EXTICR1_EXTI3_PA;

    EXTI->IMR |= EXTI_IMR_MR1;
    EXTI->RTSR |= EXTI_RTSR_TR1;
    EXTI->FTSR |= EXTI_FTSR_TR1;

    EXTI->IMR |= EXTI_IMR_MR3;
    EXTI->RTSR |= EXTI_RTSR_TR3;
    EXTI->FTSR |= EXTI_FTSR_TR3;

    NVIC_EnableIRQ(EXTI1_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);
}

void SysTick_config(void)
{
    SysTick->LOAD = 0xFFFFFF;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
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

