/* Analysis stand-in for the Arduino core: declarations only. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define PI 3.14159265f
#define A0 14
#define A1 15
#define A2 16
#define A3 17
#define A4 18
#define A5 19
#define A6 20
#define A7 21
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define F(s) (s)
void pinMode(uint8_t, uint8_t); void digitalWrite(uint8_t, uint8_t);
int digitalRead(uint8_t); int analogRead(uint8_t); void analogWrite(uint8_t, int);
unsigned long millis(void); unsigned long micros(void);
void delay(unsigned long); void delayMicroseconds(unsigned int);
struct HardwareSerial {
    void begin(unsigned long);
    int available(); int read();
    template <class T> size_t print(T); template <class T> size_t print(T, int);
    template <class T> size_t println(T); template <class T> size_t println(T, int);
    size_t println(); size_t printf(const char *, ...);
};
extern HardwareSerial Serial;
