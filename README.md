# Simplex Infrared Communication System (ESP32-S3)

I built this project to explore physical layer data transmission using light. It is a custom one-way (simplex) optical communication system that captures keystrokes from a standard USB keyboard and transmits them wirelessly via 38kHz infrared pulses. The receiver catches these pulses and displays the text on an LCD screen. 

The whole system runs on ESP32-S3 microcontrollers, written in C using the ESP-IDF framework and FreeRTOS.

## Architecture

* **Transmitter (TX):** Acts as a USB HID Host to read the physical keyboard. It translates keystrokes into specific timing durations, modulates the signal at 38kHz using the ESP32's hardware RMT peripheral, and outputs it through an IR LED.
* **Receiver (RX):** Uses a VS1838B sensor to catch and demodulate the optical signal. The RX's RMT peripheral records the pulse timings, the software decodes them back into characters, and pushes the string to an I2C LCD.

## Hardware Setup

* 2x ESP32-S3 DevKits
* 5mm IR LED + NPN Transistor (Driver)
* VS1838B IR Receiver
* 16x2 I2C LCD Screen
* Standard USB Keyboard

## Technical Notes & Bug Fixes

While building this, I had to solve a few low-level hardware and RTOS issues:

* **RMT Hardware Limits:** The ESP32-S3 glitch filter has a hard limit (3187ns). I had to manually tune `signal_range_min_ns` so the hardware wouldn't filter out my actual data pulses along with the ambient noise.
* **Stack Overflows:** Allocating a 1000-symbol RMT buffer locally caused the main task to crash. I fixed the memory architecture by statically allocating the buffer (`static rmt_symbol_word_t`) in the global RAM (BSS section).
* **FreeRTOS Synchronization:** Instead of wasting CPU cycles with polling, I implemented ISR synchronization using `xSemaphoreTake` and `xSemaphoreGiveFromISR`. The CPU essentially sleeps until the optical transmission is fully received.

## Credits & License

This project utilizes and modifies source code from the official ESP-IDF framework, specifically the `usb_host_hid` example developed by Espressif Systems. 
The original ESP-IDF framework and its examples are provided under the Apache License, Version 2.0. A copy of the license terms can be found at: http://www.apache.org/licenses/LICENSE-2.0
In compliance with the Apache License 2.0 distribution conditions, I explicitly declare that modifications were made to the original Espressif source files. The core logic of the USB HID example was altered to intercept keyboard data and redirect it into the custom infrared transmission pipeline built for this project.
