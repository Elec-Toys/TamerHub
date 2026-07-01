# Tamer Hub Repo

A fork of the OpenShock Hub firmware, but modified with a display and physical buttons for ESP32 devices. No phone needed to use it!

This is an AI vibe coded development. Mistakes may have been made but it does work for its intended purpose. Just practical and fun.

## What is added

* Display support + on-device GUI
* Physical button input
* ESP32-WROOVER-focused tweaks for better reliability

## Original project docs

Main OpenShock firmware docs are here:
https://github.com/OpenShock/Firmware

## Hardware

* ESP32 DevKit style board with at least 4MB of flash
* Generic 433 radio
* SH1106 or SSD1309 i2c display (selectable)
* Buttons and rotary encoder

See platformio.ini for specific pinouts!



## How to use it

Just flash the app and littleFS bin files onto your board, and future updates will be automatically pulled from this repo by the device.

## License

AGPLv3

