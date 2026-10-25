# Yaesu_FT891_field_CAT_control
The repo contains the code, hardware and other information for building a CAT control interface for the Yaesu 891 radio for field use

## Dependencies
Hit accept to install the dependent libraries too.
* Adafruit Tiny USB library https://github.com/adafruit/Adafruit_TinyUSB_Arduino
* Adafruit Pico PIO USB Library https://github.com/sekigon-gonnoc/Pico-PIO-USB
* Adafruit SH110X Driver Library https://github.com/adafruit/Adafruit_SH110X
* Adafruit Seesaw Library https://github.com/adafruit/Adafruit_Seesaw

## How to flash: Settings in the Arduino IDE
Everything default except
* Board: Adafruit Feather RP2040 USB Host
* CPU Speed: 240MHz (Overclock)
* Optimize: Optimize (-O)
* USB Stack: Adafruit TinyUSB


## Bitmaps and other display symbols
https://github.com/STEMpedia/eviveProjects/blob/master/imageToFlashMemoryIconsForTFT/tftIcons/tftIcons.ino
https://icons8.com/icons/set/computer
https://mischianti.org/images-to-byte-array-online-converter-cpp-arduino/

## Known issue with the Adafruit Seesaw boards
There is a firmware bug in the original firmware, as mentioned here: https://forums.adafruit.com/viewtopic.php?t=208698
Unfortunately, Adafruit didn't have enough time since 2023 to reprogram the Seesaws they have in stock. I bought three seesaw boards directly from Adafruit and they all had a 2023 datecode in the FW. 
But we can reprogram them ourselves:
* you need an FTDI programmer
* connect the RX to TX with a 1K resistor
* Connect the RX pin to the programming pin on the seesaw (it's the last pin, unmarked, aright next to the INT pin)
* Connect Vin to +5V and GND to GND
* Set the switch on the FTDI to 3.3v programming level (although I accidentally left it on 5V for one seesaw board and nothing bad happened)
* In the Arduino IDE you need to download the Atiny boards. Add http://drazzy.com/package_drazzy.com_index.json in the Arduino settings, then in the board manager, look for "megatinycore" which contains the Atiny817 board we need to flash
* The latest firmware for the seesaw with rotary encoders is in this repo: https://github.com/adafruit/Adafruit_seesawPeripheral. You need to install it as a library from a .zip file at the writing of this README
* From Adafruit_seesawperipheral examples, chose example_PID5752 (very important, don't flash another FW by accident)
* Use the board settings in https://forums.adafruit.com/viewtopic.php?t=208698 at post https://forums.adafruit.com/download/file.php?id=104774&sid=c81b0fc3ae93438ba67040ab41e0d6c5. Important is:
*   Chip: Atiny817
*   Programmer: Serial UPDI - Slow: 57600 baud
* Program the seesaw and verify with our code in the setup function. THere is a commented out bit of code that checks the correct firmware version and prints out the datecode of the FW
