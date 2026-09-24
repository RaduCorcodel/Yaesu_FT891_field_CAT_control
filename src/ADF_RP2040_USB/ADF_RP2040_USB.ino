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
// I2C Rotary encoder breakout
#include "Adafruit_seesaw.h"
#include <seesaw_neopixel.h>

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

// I2C rotary encoder breakout
#define SS_NEO_PIN       18
#define SS_ENC0_SWITCH   12
#define SS_ENC1_SWITCH   14
#define SS_ENC2_SWITCH   17
#define SS_ENC3_SWITCH   9
#define SEESAW_ADDR      0x49
Adafruit_seesaw ss = Adafruit_seesaw(&Wire);
seesaw_NeoPixel ss_pixels = seesaw_NeoPixel(4, SS_NEO_PIN, NEO_GRB + NEO_KHZ800);
int32_t enc_positions[4] = {5, 5, 0, 0};
#define SS_INT_PIN 3 //Interrupt pin on the Seesaw. SS_INT_PIN and FTH_INT_PIN are connected by a wire
#define FTH_INT_PIN 10 //Interrupt pin on the Feather. SS_INT_PIN and FTH_INT_PIN are connected by a wire

// Globals
bool is_init = false;
char cmd_key[][8] = { // The command we'll be sending and receiving from the radio
  "EX1601", //Set and Get SSB power in HF. Set "EX1601005;" Get "EX1601;"
  "EX1603", //Set and Get CW power in HF. Set "EX1603005;" Get "EX1603;"
  "MS;",     //Meter select on the radio front panel. Set "MS02;" Get "MS;"
  "BS;",     //Band select. Set "BS02;" Get N/A
  "BU0;",    //Band-up. Write only "BU0;"
  "BD0;"     //Band-down. Write only "BD0;"
};
size_t cmd_val[4] = { //The responses we're expecting from the radio
  0,        // SSB power
  0         // CW power (also the power the tuner will use)
}; 
unsigned long curr_millis = 0; //We track the time in the main loop without blocking it. We use this for poling the radio
unsigned long prev_millis = 0;
unsigned int  poling_interval = 1000;

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
    //if (!is_init) {is_init=true;}
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

//ToD0: replace hard coded numbers
void DecodeSerialMessage(const char* identifier, char* msg, size_t* res) {
  if (strncmp(identifier, msg, 6) == 0) { //We compare the command string with the received. The root (6 characters) should match
    char c_res[5];
    memcpy(c_res,&msg[6], 3); //Copy a 3 character substring from position 6 
    *res = atoi(c_res); //Convert character array to numeric value
    //Serial.print(c_res);Serial.println("~");
  }
}

// void DisplayPrintText(char* text, size_t x, size_t y){
//   display.clearDisplay();
//   display.setTextSize(1.5);
//   display.setTextColor(SH110X_WHITE);
//   display.setCursor(x, y);
//   display.print(text);
//   display.display();
// }

void DisplayUpdateUI(void){
  // // Get updates from radio if we made any changes using the usual radio controls
    // for (size_t i=0; i<2; ++i) {
    //   HostSetMessage(strlen(cmd_key[i]), cmd_key[i]);
    //   size_t n_bytes;
    //   char inbound_msg[16];
    //   HostGetMessage(&n_bytes, inbound_msg);
    //   DecodeSerialMessage(cmd_key[i], inbound_msg, &cmd_val[i]);
    // }
  display.clearDisplay();
  display.setTextSize(1.5);
  display.setTextColor(SH110X_WHITE);
  char display_text[16];
  sprintf(display_text, "SSB POWER %3.dW", cmd_val[0]);
  display.setCursor(0, 0);
  display.print(display_text);
  sprintf(display_text, "CW POWER  %3.dW", cmd_val[1]);
  display.setCursor(0, 15);
  display.print(display_text);
  display.display();
}

bool OnUpdateEncoder(void){
  bool should_update = false;
  for (size_t e=0; e<4; e++) {
    int32_t new_enc_position = ss.getEncoderPosition(e);
    // did we move around?
    if (enc_positions[e] != new_enc_position) {
      switch(e){
        case 0:
        case 1:
          enc_positions[e] = min(100, max(5, new_enc_position)); //Clamp value
          break;
        case 2:
        case 3:
          enc_positions[e] = min(5, max(0, new_enc_position)); //Clamp value
          break;
      }
      if (enc_positions[e] != new_enc_position) { //We clamped the value, need to rewrite the internal state of the encoder
        ss.setEncoderPosition(enc_positions[e],e);
      }
      else { //New encoder value is valid so send a message to the radio to update
        should_update = true;
        char buf[16];
        sprintf(buf, "New encoder vals: %d %d %d %d", enc_positions[0], enc_positions[1], enc_positions[2], enc_positions[3]);
        Serial.println(buf);
      }
      // change the neopixel color, mulitply the new positiion by 4 to speed it up
      ss_pixels.setPixelColor(e, Wheel((new_enc_position*4) & 0xFF));
      ss_pixels.show();
    }
  }
  return should_update;
}

uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85) {
    return seesaw_NeoPixel::Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if (WheelPos < 170) {
    WheelPos -= 85;
    return seesaw_NeoPixel::Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return seesaw_NeoPixel::Color(WheelPos * 3, 255 - WheelPos * 3, 0);
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

  // Start the I2C rotary encoder breakout board (it has its own Atiny85)
  if (! ss.begin(SEESAW_ADDR) || !ss_pixels.begin(SEESAW_ADDR)) {
    Serial.println("Couldn't find seesaw on default address");
    while(1) delay(10);
  }
  Serial.println("Seesaw started!");
  uint32_t version = ((ss.getVersion() >> 16) & 0xFFFF);
  if (version  != 5752){
    Serial.print("Wrong firmware loaded? "); Serial.println(version);
    while(1) delay(10);
  }
  ss.pinMode(SS_ENC0_SWITCH, INPUT_PULLUP);
  ss.pinMode(SS_ENC1_SWITCH, INPUT_PULLUP);
  ss.pinMode(SS_ENC2_SWITCH, INPUT_PULLUP);
  ss.pinMode(SS_ENC3_SWITCH, INPUT_PULLUP);
  ss.setGPIOInterrupts(1UL << SS_ENC0_SWITCH | 1UL << SS_ENC1_SWITCH | 
                       1UL << SS_ENC2_SWITCH | 1UL << SS_ENC3_SWITCH, 1);
  // get starting positions
  for (size_t e=0; e<4; e++) {
    ss.setEncoderPosition(enc_positions[e], e);
    ss.enableEncoderInterrupt(e);
  }
  ss_pixels.setBrightness(255);
  ss_pixels.show(); // Initialize all pixels to 'off'
  Serial.println("Finished with the rotary encoder startup");
  pinMode(FTH_INT_PIN, INPUT_PULLUP);
  uint32_t mask = ((uint32_t)0b1 << SS_INT_PIN);
  ss.pinModeBulk(mask, INPUT_PULLUP);
  ss.setGPIOInterrupts(mask, 1);

  // Double check the new firmware is loaded on the seesaw board
  /// Comment-out in the release version of the code
  version = ss.getVersion();
  uint16_t product = version >> 16 & 0xFFFF;
  uint16_t date = version & 0xFFFF;
  uint8_t day = date >> 11 & 0x1F;
  uint8_t month = date >> 7 & 0x0F;
  uint8_t year =  date & 0x7F;
  Serial.print("Seesaw found at 0x"); Serial.println(SEESAW_ADDR, 16);
  Serial.println("==========================");
  Serial.print("version code = 0x"); Serial.println(version, 16);
  Serial.println("--------------------------");
  Serial.print("product = "); Serial.println(product);
  Serial.print("year = "); Serial.println(year);
  Serial.print("month = "); Serial.println(month);
  Serial.print("day = "); Serial.println(day);
  Serial.println("==========================");
}

void TestCmdSeq(size_t v) {
  char buf[10];
  // memcpy(buf,cmd_key[v], 6); //Copy a 6 character substring from position 0 
  sprintf(buf,"%s%03d;",cmd_key[v],cmd_val[v]);
  //erial.print(buf);Serial.println(strlen(buf));
  HostSetMessage(strlen(buf), buf);
}

void loop() {

  
  // curr_millis = millis();
  // if(curr_millis - prev_millis >= poling_interval) {
  //   prev_millis = curr_millis;
  //   DisplayUpdateUI();
  // }

  // Update the display
  DisplayUpdateUI();

  // Weird way of using interrupts but that's the price we pay for using a ready-made breakout board
  //// Read the encoders from the I2C only if we observed an interrupt (so no polling the I2C bus, save on compute)
  if(!digitalRead(FTH_INT_PIN)){
    bool update_radio = OnUpdateEncoder();
    if(update_radio) {
      for(size_t e=0; e<2; ++e) {
        if(cmd_val[e] != enc_positions[e]) {
          cmd_val[e] = enc_positions[e]; //Sync the radio commanded values to the desired values from encoders
          TestCmdSeq(e);
        }
      }
    }
  }

  // Also pass-through commands from the computer serial monitor to the radio
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

void DisplayPrintText(char* text, size_t x, size_t y){
  display.clearDisplay();
  display.setTextSize(1.5);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(x, y);
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
    //char* msg = cmd[1];
    HostSetMessage(7, cmd[1]); //c_msg
    //Receive message
    size_t n_bytes;
    char inbound_msg[16];
    HostGetMessage(&n_bytes,inbound_msg);
    // Process message
    size_t n_res;
    DecodeSerialMessage(cmd[1], inbound_msg, &n_res); //c_msg
    // Update display
    char display_text[16];
    sprintf(display_text, "SSB Power %dW", n_res);
    DisplayPrintText(display_text, 0, 0);
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

