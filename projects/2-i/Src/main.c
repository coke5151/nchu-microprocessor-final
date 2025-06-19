#include "stm32f10x.h"

void delay_us(uint16_t t);
void delay_ms(uint16_t t);

int main()
{
    RCC->APB2ENR |= 0xFC; // 啟用 GPIO 連接埠的 clock

    GPIOA->CRL = 0x33333333; // PA0 to PA7 outputs
    GPIOA->CRH = 0x33333333; // PA8 to PA15 outputs

    delay_ms(100); // 緩衝 100ms

    GPIOA->ODR = 1 << 0; // PA0 on
    delay_us(10);
    GPIOA->ODR = 0 << 0; // PA0 off
}

void delay_us(uint16_t t)
{
    volatile unsigned long l = 0;

    for (uint16_t i = 0; i < t; i++)
        for (l = 0; l < 7; l++)
            ;
}
void delay_ms(uint16_t t)
{
    volatile unsigned long l = 0;

    for (uint16_t i = 0; i < t; i++)
        for (l = 0; l < 9000; l++)
            ;
}
