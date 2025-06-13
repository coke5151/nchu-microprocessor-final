#include "stm32f10x.h"

void delay_ms(uint16_t t);

int main()
{
    RCC->APB2ENR |= 0xFC; // 啟用 GPIO 連接埠的 clock
    RCC->APB1ENR |= (1 << 0);
    GPIOA->CRL |= 0xB;
    TIM2->CCER = 0x1;
    TIM2->CCMR1 |= 0x60;
    TIM2->PSC = 72 - 1;
    TIM2->ARR = 20000 - 1;
    TIM2->CCR1 = 1500; // 初始值設定為中間位置
    TIM2->CR1 = 1;

    while (1) {
        // 順時針轉至 180 度
        TIM2->CCR1 = 2000;
        delay_ms(10000);

        // 逆時針轉至 0 度
        TIM2->CCR1 = 1000;
        delay_ms(10000);

        //		// 返回中心位置
        //		TIM2->CCR1 = 1500;
        //		delay_ms(10000);
    }
}

void delay_ms(uint16_t t)
{
    volatile unsigned long l = 0;

    for (uint16_t i = 0; i < t; i++)
        for (l = 0; l < 9000; l++)
            ;
}
