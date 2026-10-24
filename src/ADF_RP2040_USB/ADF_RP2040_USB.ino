/*********************************************************************
(c) 2026 Radu Corcodel, Brookline MA-02445
Inspired from "Adafruit TinyUSB LibraryDualRole/CDC/serial_host_bridge.ino" example
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
// Project includes
#include "icons.h"

// CDC USB Host object
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
#define SS_ENC0_SWITCH   12
#define SS_ENC1_SWITCH   14
#define SS_ENC2_SWITCH   17
#define SS_ENC3_SWITCH   9
#define SEESAW_ADDR      0x49
Adafruit_seesaw ss = Adafruit_seesaw(&Wire);
#define SS_INT_PIN 3 //Interrupt pin on the Seesaw. SS_INT_PIN and FTH_INT_PIN are connected by a wire
#define FTH_INT_PIN 10 //Interrupt pin on the Feather. SS_INT_PIN and FTH_INT_PIN are connected by a wire

// Globals
char cmd_key[][8] = { // The command we'll be sending and receiving from the radio
  "EX1601", //Set and Get SSB power in HF. Set "EX1601005;" Get "EX1601;"
  "EX1603", //Set and Get CW power in HF. Set "EX1603005;" Get "EX1603;"
  "MS",     //Meter select on the radio front panel. Set "MS02;" Get "MS;"
  "BU0",    //Band-up. Write only "BU0;"
  "BD0"     //Band-down. Write only "BD0;"
    // "BS;",     //Band select. Set "BS02;" Get N/A
};
char cmd_send[16] = "";
size_t cmd_len[4] = { //The length of the argument we expect from the radio for each command
  3, //e.g. "EX1601005;"
  3, //e.g. "EX1603005;"
  1, //e.g. "MS2;"
  1
};
size_t enc_positions[4] = { //The responses we're expecting from the radio
  5,        // SSB power
  5,        // CW power (also the power the tuner will use)
  0,        // Meter ID (0=CMP, 1=ALC, 2=PO, 3=SWR, 4=IDD)
  0
}; 
unsigned long curr_millis = 0; //We track the time in the main loop without blocking it. We use this for poling the radio
unsigned long prev_millis = 0;
unsigned int  radio_poling_interval = 5000; //[ms]
// unsigned int  radio_freq = 10;

// forward messages PCSerial <-> SerialHost
void forward_serial(void) {
  uint8_t buf[64];

  // PCSerial -> SerialHost
  if (Serial.available()) {
    size_t count = Serial.read(buf, sizeof(buf));
    if (SerialHost && SerialHost.connected()) {
      SerialHost.write(buf, count);
      SerialHost.flush();
    }
  }

  // SerialHost -> PCSerial
  if (SerialHost.connected() && SerialHost.available()) {
    size_t count = SerialHost.read(buf, sizeof(buf));
    Serial.write(buf, count);
    Serial.flush();
  }
}

// Helper method for getting a message from the SerialHost
void HostGetMessage(size_t *n, char* msg) {
  uint8_t buf[16];
  if (SerialHost.connected() && SerialHost.available()) {
    size_t count = SerialHost.read(buf, sizeof(buf));
    *n = count;
    for (size_t i=0; i<count ; i++) {
      msg[i] = buf[i];
    }
    Serial.print("Received message: "); Serial.println((char*)buf);
    Serial.flush();
  }
} 

// Helper method for sending a message to the SerialHost
void HostSetMessage(byte msg_sz, const char* msg) {
  if (SerialHost && SerialHost.connected()) {
    SerialHost.write(msg, msg_sz);
    SerialHost.flush();
    delay(50); //30 Wait for the radio to compute the response and send back
    Serial.print("Sent message: "); Serial.println(msg);
  }
}

// Specialized method for decoding a message  oming from a Yaesu radio using CAT control
// Messages come in the form "IDENxxx;" where IDEN is some command identifier, xxx is usually a numeric value (can be anywhere between 3 to 1 characters), and a mandatory semicolon to mark the end of the message
void DecodeSerialMessage(const char* identifier, char* msg, size_t c_len, size_t* res) {
  // Serial.print("Characters to match "); Serial.println(strlen(identifier)-1);
  char c_res[32];
  if (strncmp(identifier, msg, strlen(identifier)-1) == 0) { //We compare the command string with the received. The root should match
    //This memcpy just doesn't get compiled well on the Arduino so we copy the bytes manually
    // memcpy(c_res,&msg[strlen(identifier)-1], 3); //Eg: "EX1601011;" Copy a 3 character substring from position 6 
    for(size_t c=0; c<c_len; ++c) {
      c_res[c] = msg[c+strlen(identifier)-1];
    }
    *res = atoi(c_res); //Convert character array to numeric value
    Serial.print(msg);Serial.print("~");Serial.print(c_res);Serial.println("~");
  }
}

// Radio poling methods:
// Request data from device to update the internal state of our controller
//// We call this once when the controller starts and periodically while running
void ReqDeviceUpdate(void) {
  // Get updates from radio if we made any changes using the usual radio controls
  char buf[10];
  size_t n_bytes;
  char inbound_msg[16];
  for (size_t i=0; i<3; ++i) { // Only the first 3 controlls can be querried, the forth doesn't have a read command
    sprintf(buf,"%s;",cmd_key[i]);
    HostSetMessage(strlen(buf), buf);
    HostGetMessage(&n_bytes, inbound_msg);
    DecodeSerialMessage(buf, inbound_msg, cmd_len[i], &enc_positions[i]);
    ss.setEncoderPosition(enc_positions[i],i); //Update the internal state of the encoder
  }
  // // Show the current frequency on the display why not?!
  // char freq_msg[3] = "FA";
  // sprintf(buf,"%s;",freq_msg);
  // HostSetMessage(strlen(buf),buf);
  // HostGetMessage(&n_bytes, inbound_msg);
  // DecodeSerialMessage(freq_msg, inbound_msg, 9, &radio_freq);
}

// Display methods
// Update the OLED display with the new values
void DisplayUpdateUI(size_t *enc){
  display.clearDisplay();
  display.setTextSize(1.5);
  display.setTextColor(SH110X_WHITE);
  char display_text[32];
  sprintf(display_text, "SSB POWER %3.dW", enc[0]);
  display.setCursor(20, 26);
  display.print(display_text);
  sprintf(display_text, "CW POWER  %3.dW", enc[1]);
  display.setCursor(20, 41);
  display.print(display_text);
  // sprintf(display_text, "Freq  %d", radio_freq);
  // display.setCursor(20, 56);
  // display.print(display_text);
  // Draw the PC icon on the display if the computer is connected 
  if(Serial){
    display.drawBitmap(0,0,icon_pc,18,18,SH110X_WHITE);
  }
  // Draw the Radio icon if the radio is connected
  if (SerialHost && SerialHost.connected()) { //SerialHost && SerialHost.connected()
    display.drawBitmap(26,0,icon_radio,18,18,SH110X_WHITE);
  }

  // Flush everything to the display
  display.display();
}

// Encoder methods:
// Check if we have new encoder values we should pass to the radio
bool OnUpdateEncoder(void){
  bool should_update = false;
  for (size_t e=0; e<4; e++) {
    size_t new_enc_position = ss.getEncoderPosition(e);
    // did we move around?
    if (enc_positions[e] != new_enc_position) {
      switch(e){
        case 0:
        case 1:
          enc_positions[e] = min(100, max(5, new_enc_position)); //Clamp value or assign valid value
          sprintf(cmd_send,"%s%03d;",cmd_key[e],enc_positions[e]); //Compose the command here to send to radio
          break;
        case 2:
          enc_positions[e] = min(4, max(0, new_enc_position));
          sprintf(cmd_send,"%s%01d;",cmd_key[e],enc_positions[e]); //Compose the command here to send to radio
          break;
        case 3: //No modifier but compare if we want to go band up or down
          if(enc_positions[e] < new_enc_position) {
            sprintf(cmd_send,"%s;",cmd_key[e]);
          }
          else {
            sprintf(cmd_send,"%s;",cmd_key[e+1]);
          }
          enc_positions[e] = new_enc_position;
          break;
      }
      if (enc_positions[e] != new_enc_position) { //We clamped the value, need to rewrite the internal state of the encoder on the Seesaw
        ss.setEncoderPosition(enc_positions[e],e);
      }
      else { //New encoder value is valid so send a message to the radio to update
        should_update = true;
        char buf[16];
        sprintf(buf, "New encoder vals: %d %d %d %d", enc_positions[0], enc_positions[1], enc_positions[2], enc_positions[3]);
        Serial.println(buf);
        Serial.print("Will send: ");Serial.print(cmd_send);Serial.println("~");
      }
    }
  }
  return should_update;
}


#if defined(ARDUINO_ARCH_RP2040)
//--------------------------------------------------------------------+
// For RP2040 use both core0 for device stack, core1 for host stack
//--------------------------------------------------------------------+

//------------- Core0 -------------//
void setup() {
  Serial.begin(115200);
  int native_usb_timeout = 1000;
  prev_millis = millis();
  while ( !Serial ) {
    delay(10);   // wait for native usb
    curr_millis = millis();
    if(curr_millis - prev_millis >= native_usb_timeout) {
      break;
    }
  }
  if(Serial){
    Serial.println("------------------------------");
    Serial.println("Yaesu-891 CAT control starting");
  }
  

  // Start OLED
  display.begin(0, true); // we dont use the i2c address but we will reset!
  display.display();
  delay(100);
  display.clearDisplay();
  if(Serial){
    Serial.println("Started SPI display");
  }

  // Start the I2C rotary encoder breakout board (it has its own Atiny85)
  if (! ss.begin(SEESAW_ADDR)) {
    Serial.println("Couldn't find seesaw on default address");
    while(1) delay(10);
  }
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
  pinMode(FTH_INT_PIN, INPUT_PULLUP);
  uint32_t mask = ((uint32_t)0b1 << SS_INT_PIN);
  ss.pinModeBulk(mask, INPUT_PULLUP);
  ss.setGPIOInterrupts(mask, 1);
  // // Double check the new firmware is loaded on the seesaw board
  // /// Comment-out in the release version of the code
  // version = ss.getVersion();
  // uint16_t product = version >> 16 & 0xFFFF;
  // uint16_t date = version & 0xFFFF;
  // uint8_t day = date >> 11 & 0x1F;
  // uint8_t month = date >> 7 & 0x0F;
  // uint8_t year =  date & 0x7F;
  // Serial.print("Seesaw found at 0x"); Serial.println(SEESAW_ADDR, 16);
  // Serial.println("==========================");
  // Serial.print("version code = 0x"); Serial.println(version, 16);
  // Serial.println("--------------------------");
  // Serial.print("product = "); Serial.println(product);
  // Serial.print("year = "); Serial.println(year);
  // Serial.print("month = "); Serial.println(month);
  // Serial.print("day = "); Serial.println(day);
  // Serial.println("==========================");
  if(Serial){
    Serial.println("Started rotary encoder board");
  }

  ReqDeviceUpdate();
  if(Serial){
    Serial.println("Ready");
  }
}

void loop() {

  // From time to time, poll the radio to see if we didn't change anything using the radio instead of this control box
  // Keep this commented out during development otherwise you have a bunch of unnecessary and annoying traffic to see of the serial port
  curr_millis = millis();
  if(curr_millis - prev_millis >= radio_poling_interval) {
    prev_millis = curr_millis;
    ReqDeviceUpdate();
  }

  // Update the display
  //ToDo: consider updating less often either with millis or event triggered
  DisplayUpdateUI(enc_positions);

  // Weird way of using interrupts but that's the price we pay for using a ready-made breakout board
  //// Read the encoders from the I2C only if we observed an interrupt (so no polling the I2C bus, save on compute)
  //// Well... the interrupt doesn't work at all, it's triggering all the time. We will fix later, and now just poll the I2C line at every iteration
  // if(!digitalRead(FTH_INT_PIN)){
    //bool update_radio = OnUpdateEncoder();
    if(OnUpdateEncoder()) {
      HostSetMessage(strlen(cmd_send), cmd_send);
    }
  // }

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
