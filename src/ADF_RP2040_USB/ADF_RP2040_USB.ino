/*********************************************************************
Based on "Adafruit TinyUSB LibraryDualRole/CDC/serial_host_bridge.ino" example
This program only works on the Adafruit RP2040-USB-host (built-in USB Type-A host)
*********************************************************************/

/* This example demonstrates use of both device and host, where
 * - Device run on native usb controller (roothub port0)
 * - Host depending on MCUs run on rp2040: bit-banging 2 GPIOs with the help of Pico-PIO-USB library (roothub port1)
 *
 * Requirements:
 * - For rp2040:
 *   - [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) library
 *   - 2 consecutive GPIOs: D+ is defined by PIN_USB_HOST_DP, D- = D+ +1 (defined in usb_helper.h) This board has D+ 16 and D- 17
 *   - Provide VBus (5v) (Pin 18) and GND for peripheral
 *   - CPU Speed must be either 120 or 240 Mhz. Selected via "Menu -> CPU Speed". Mine worked only with 240MHz
 *   - Pins GPIO16, GPIO17, GPIO18 are reserved for the USB Host
 *   - Pins GPIO20, GPIO21 are reserved for the NeoPixel indicator

 */

// General includes
#include <iostream>
#include <stdio.h>
// USB related includes
#include <stdexcept>
#include "usb_helper.h" // USBHost is defined in usb_helper.h
// OLED-Display related includes
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// CDC Host object
Adafruit_USBH_CDC SerialHost;

// SPI Display object (SPI1 // SPI0)
#define OLED_MOSI     27 // 3
#define OLED_CLK      26 // 2
#define OLED_DC       24 // 26
#define OLED_CS       29 // 1
#define OLED_RST      25 // 27
#define H_RES         128
#define V_RES         64
Adafruit_SH1106G display = Adafruit_SH1106G(H_RES, V_RES, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);

// Globals
bool is_init = false;


// forward Seral <-> SerialHost
void forward_serial(void) {
  uint8_t buf[64];

  // Serial -> SerialHost
  if (Serial.available()) {
    size_t count = Serial.read(buf, sizeof(buf));
    if (SerialHost && SerialHost.connected()) {
      SerialHost.write(buf, count);
      SerialHost.flush();
    }
  }

  // SerialHost -> Serial
  if (SerialHost.connected() && SerialHost.available()) {
    size_t count = SerialHost.read(buf, sizeof(buf));
    Serial.write(buf, count);
    Serial.flush();
  }
}

void HostGetMessage(size_t *n, char* msg) {
  uint8_t buf[10];
  if (SerialHost.connected() && SerialHost.available()) {
    size_t count = SerialHost.read(buf, sizeof(buf));
    *n = count;
    for (size_t i=0; i<count ; i++) {
      msg[i] = buf[i];
    }
    Serial.print("Received message: "); Serial.println((char*)buf);
    if (!is_init) {is_init=true;}
  }
} 

void HostSetMessage(byte msg_sz, const char* msg) {
  if (SerialHost && SerialHost.connected()) {
    SerialHost.write(msg, msg_sz);
    SerialHost.flush();
    delay(25); //30 Wait for the radio to compute the response and send back
    Serial.print("Sent message: "); Serial.println(msg);
  }
}

void DecodeSerialMessage(const char* identifier, char* msg, size_t* res) {
  if (strncmp(identifier, msg, 6) == 0) { //We compare the command string with the received. The root (6 characters) should match
    char c_res[5];
    memcpy(c_res,&msg[6], 3); //Copy a 3 character substring from position 6 
    *res = atoi(c_res); //Convert character array to numeric value
    //Serial.print(c_res);Serial.println("~");
  }
}

void DisplayPrintText(char* text){
  display.clearDisplay();
  display.setTextSize(1.5);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print(text);
  display.display();
}

#if defined(ARDUINO_ARCH_RP2040)
//--------------------------------------------------------------------+
// For RP2040 use both core0 for device stack, core1 for host stack
//--------------------------------------------------------------------+

//------------- Core0 -------------//
void setup() {
  Serial.begin(115200);
  while ( !Serial ) delay(10);   // wait for native usb
  Serial.println("TinyUSB Host Serial Echo Example");

  // Start OLED
  display.begin(0, true); // we dont use the i2c address but we will reset!
  display.display();
  delay(100);
  display.clearDisplay();
  // display.drawPixel(10, 10, SH110X_WHITE);
  // display.display();
  // delay(2000);
  // display.clearDisplay();
  // display.setTextSize(1.5);
  // display.setTextColor(SH110X_WHITE);
  // display.setCursor(0, 0);
  // display.print("SSB Power");
  // display.display();
}

void loop() {
  if(!is_init) {
    //Send message
    char const c_msg[8] = "EX1601;";
    HostSetMessage(7, c_msg);
    //Receive message
    size_t n_bytes;
    char inbound_msg[16];
    HostGetMessage(&n_bytes,inbound_msg);
    // Process message
    size_t n_res;
    DecodeSerialMessage(c_msg, inbound_msg, &n_res);
    // Update display
    char display_text[16];
    sprintf(display_text, "SSB Power %dW",n_res);
    DisplayPrintText(display_text);
  }
  forward_serial();
}

//------------- Core1 -------------//
void setup1() {
  // configure pio-usb: defined in usbh_helper.h
  rp2040_configure_pio_usb();

  // run host stack on controller (rhport) 1
  // Note: For rp2040 pico-pio-usb, calling USBHost.begin() on core1 will have most of the
  // host bit-banging processing works done in core1 to free up core0 for other works
  USBHost.begin(1);

  // Initialize SerialHost
  SerialHost.begin(9600);
}

void loop1() {
  USBHost.task();
}

#else
  throw std::invalid_argument("Code designed only for the Adafruit RP2040-USB-host");
#endif

//--------------------------------------------------------------------+
// TinyUSB Host callbacks
//--------------------------------------------------------------------+
extern "C" {

// Invoked when a device with CDC interface is mounted
// idx is index of cdc interface in the internal pool.
void tuh_cdc_mount_cb(uint8_t idx) {
  // bind SerialHost object to this interface index
  SerialHost.mount(idx);
  Serial.println("SerialHost is connected to a new CDC device");
}

// Invoked when a device with CDC interface is unmounted
void tuh_cdc_umount_cb(uint8_t idx) {
  SerialHost.umount(idx);
  Serial.println("SerialHost is disconnected");
}

}

