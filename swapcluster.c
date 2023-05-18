
#define F_CPU 16000000UL    // Тактовая частота 16МГц

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdio.h>          // для sprintf()

#define UART_BAUD_RATE 57600UL  // Скорость передачи UART 57600 бит/с
#define SET_UBRR ((F_CPU/(16UL*UART_BAUD_RATE))-1UL) // Значения регистров для настройки скорости UART

#define IMP_IN 3    // Количество импульсов на 1 оборот на выходе
#define IMP_OUT 2   // Требуемое для тахометра количество импульсов на 1 оборот

uint16_t period = UINT16_MAX;   // измеренный период сигнала в 1/62500с
uint8_t time = 0;               // используется в главном цикле
uint8_t setup_mode = 0;

// Настроить UART
void init_uart () {
    UBRR0H = (uint8_t)(SET_UBRR>>8);        // настройка скорости
    UBRR0L = (uint8_t)SET_UBRR;
    UCSR0B |= (1<<TXEN0);                   // разрешение передачи
    UCSR0B |= (1<<RXEN0);                   // разрешение приёма
    UCSR0B |= (1<<RXCIE0);                  // включение прерывания по приёму
    UCSR0C |= (1<<UCSZ00) | (1<<UCSZ01);    // 8 бит
}

// Отправить символ в UART
void uart_putc (uint8_t data) {
    while (!(UCSR0A & (1<<UDRE0))); // ожидание готовности
    UDR0 = data;                    // отправка
}

// Отправить строку в UART
void uart_puts (char *s) {
    while (*s) {        // пока строка не закончилась
        uart_putc(*s);  // отправить очередной символ
        s++;            // передвинуть указатель на следующий символ
    }
}

// Отправить строку из памяти программ в UART
void uart_puts_P (const char *progmem_s) {
    register char c;
    while ((c = pgm_read_byte(progmem_s++))) {      // пока строка не закончилась
        uart_putc(c);                               // отправить очередной символ
    }
}

// принять символ из UART
char uart_getc () {
    while (!(UCSR0A & (1<<RXC0)));
    return UDR0;
}

// Настроить таймер 1 (измерение частоты)
void init_meter () {
    TCCR1B |= (1<<ICES1);   // захват положительного фронта сигнала
    TIMSK1 |= (1<<ICIE1);   // включить прерывание по захвату
    TIMSK1 |= (1<<TOIE1);   // включить прерывание по переполнению
    TCCR1B |= (1<<CS12);    // делитель 256, частота 62500Гц
}

// Настроить таймер 3 (генератор выходного сигнала)
void init_gen () {
    DDRD |= (1<<PD2);           // сделать PD2 выходом
    PORTD |= (1<<PD2);          // для использования таймером, на этом выходе должна быть единица
    TCCR3A |= (1<<COM3B1);      // режим работы выхода OC3B: неинвертирующий
    // Режим работы таймера: Fast PWM со счётом до OCR3A, скважность в OCR3B
    TCCR3A |= (1<<WGM31) | (1<<WGM30);
    TCCR3B |= (1<<WGM33) | (1<<WGM32);
}

// Настроить таймер 4 (главный цикл)
void init_timer_main () {
    TCCR4B |= (1<<WGM42);   // режим сравнения
    OCR4A = 6250;           // отсчёт до 6250, 1/10с
    TIMSK4 |= (1<<OCIE4A);  // включить прерывание по сравнению
    TCCR4B |= (1<<CS42);    // делитель 256, частота 62500Гц
}

// Задать частоту генератора
void set_freq (uint16_t freq) {
    if (freq == 0) TCCR3B &= ~(1<<CS32);    // если заданная частота = 0, остановить генератор
    else {
        // Запустить генератор, если не запущен. Делитель 256, тактовая частота 62500Гц
        if (!(TCCR3B & 0b00000111)) TCCR3B |= (1<<CS32);
        OCR3A = (62500 / freq) - 1; // период импульсов в 62500-х долях секунды
        OCR3B = OCR3A / 2;          // для скважность 50% здесь половина от OCR3A
    }
}

// Прерывание по захвату сигнала таймером 1
ISR (TIMER1_CAPT_vect) {
    TCNT1 = 0;      // обнулить счётный регистр
    period = ICR1;  // результат измерения берётся из регистра захвата
}

// Прерывание по переполнению таймера 1
ISR (TIMER1_OVF_vect) {
    period = UINT16_MAX;
}

// Прерываение по сравнению таймера 4
ISR (TIMER4_COMPA_vect) {
    time++;
}

int main () {

    uint16_t frequency = 0; // частота входного сигнала
    uint16_t freq_out = 0;  // частота на выходе
    uint16_t rpm = 0;       // обороты двигателя
    char buffer [30];       // буфер для вывода строк
    
    init_uart();        // настроить UART
    init_meter();       // настроить таймер 1 (измерение частоты)
    init_gen();         // настроить таймер 3 (генератор сигнала)
    init_timer_main();  // настроить таймер 4 (главный цикл)
    
    uart_puts_P(PSTR("\x1b[2J\x1b[?25l\n\n"));
    uart_puts_P(PSTR("Преобразователь частоты тахометра\n\n\r"));
    uart_puts_P(PSTR("вход\tвыход\tоб/мин\n\r"));
    
    sei();  // разрешить все прерывания
    
    while (1) {
        if (time) {
            frequency = 62500 / period;
            freq_out = (frequency / IMP_IN) * IMP_OUT;
            rpm = (frequency * 60) / IMP_IN;
            set_freq(freq_out);
            sprintf(buffer, "%u   \t%u   \t%u    \r", frequency, freq_out, rpm);
            uart_puts(buffer);
            time = 0;
        }
        asm(" ");
    }
    
}
