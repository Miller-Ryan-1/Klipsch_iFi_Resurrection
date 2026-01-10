/*
This code is used to replace the functions of the Klipsch iFi 2.1
system's dock.  Loss of dock function represents a critical failure point for 
this speaker system without the following code, along with some additional
hardware discussed here [[link to github repo with additional info]]

It uses the Serial Monitor to submit the commands originally sent over by the
dock's logic. Thus, a serial connection to actual chip is required to set the volume
(to be updated in next version!). The commands can be found by typing "Help" into the
Serial.

* * * * * Update * * * * *
In addition to serial monitor commands, user friendlier buttons placed on the device case
can now be used to increment or decrement the attentuation levels, one set for the satellite
speaker volume, and one set for the sub volume (effectively bass control).
* * * * * * * * * * * * * *

Important Notes:
  The OEM dock interacts with a LM1973M chip on the PreAmp Board.  It sends
  bit (1s and 0s) signals to the chip to control the overall volume of the satellite 
  speakers and the subwoofer.  For this code the mappings are:
  LM1973M Pins Connected to Microcontroller (LM1973M -> Microcontroller Pin ):
    CLK   -> D4
    DATA  -> D3
    LATCH -> D2
  The pinout diagram and bit-bang info for LM1973M can be found with the supporting 
  materials.
  
  LM1973 Channels:
    0x00 = Ch1 (Sat L), 0x01 = Ch2 (Sat R), 0x02 = Ch3 (Sub)

  Default Settings:
    Satellites = 0x14 (≈ -20 dB), Sub = 0x14 (≈ -20 dB)
    *used these high attenuation setting as default due to ground loop issues to be fixed

  Serial Monitor: 9600 baud
  Commands:
    Gross Volume Control: 'MUTE' / 'FULL' / 'SATFULL' / 'SUBFULL'
    Granular Volume Control: 'SATL n' / 'SUBL n' / 'ALL n'   (where n  =0..9 => ATT_TABLE[n])
    Channel Address: 'SAT xx' / 'SUB xx' / 'ALL xx'   (hex byte, e.g., 1E, 4E, 00, FF)
    Save: 'SAVE'    (write current to EEPROM)

By: R Miller
Last Updated: Jan 2026
*/

// To store the last volume settings in memory to apply on system startup.
#include <EEPROM.h>

// ---------- Button debouncer types/prototypes (must be above functions in Arduino) ----------
struct ButtonEdge {
  uint8_t pin;
  bool stableState;              // last debounced read (true=HIGH, false=LOW)
  bool lastReading;
  unsigned long lastChangeMs;
};

// -------------------------------------------------------------------------------------------

// Map LM1973M pins to Microcontroller Pins
const uint8_t PIN_CLK  = 4;
const uint8_t PIN_DATA = 3;
const uint8_t PIN_LOAD = 2;

// Volume Control Buttons
const uint8_t V_UP_PIN   = 7;    // <-- put real pin number here
const uint8_t V_DOWN_PIN = 6;  // <-- put real pin number here
const uint8_t S_UP_PIN   = 9;    // <-- put real pin number here
const uint8_t S_DOWN_PIN = 8;  // <-- put real pin number here

// Debounce timing (ms)
const unsigned long BTN_DEBOUNCE_MS = 35;
// -----------------------------------------------------------------------------

// Map channels (L & R satellite speakers and subwoofer) to their LM1973M hex bit codes.
const uint8_t ADDR_CH1 = 0x00;
const uint8_t ADDR_CH2 = 0x01;
const uint8_t ADDR_CH3 = 0x02;

// Core volume/attenuation codes:
// !Note: 0 dB is max volume, negative numbers represent attenuation from that level
const uint8_t CODE_FULL_VOL = 0x00; // 0 dB
const uint8_t CODE_DEEPEST  = 0x4E; // -76 dB
const uint8_t CODE_MUTE_1   = 0x4F; // first mute code
const uint8_t CODE_MUTE_FF  = 0xFF; // explicit 0xFF mute

// Ten useful attenuation (volume) steps between 0x00 and 0x4F (max and first mute)
const uint8_t ATT_TABLE[10] = {
  0x06, 0x0C, 0x12, 0x18, 0x1E,
  0x24, 0x2A, 0x30, 0x3C, 0x48
};

// Defaults (first run) and presets the live “last known” in case nothing is stored
// to rewrite it later:
const uint8_t DEFAULT_SAT = 0x10; // 16  -> ~ -8 dB
const uint8_t DEFAULT_SUB = 0x10; // 16   -> ~ -8 dB
uint8_t lastSat = DEFAULT_SAT;
uint8_t lastSub = DEFAULT_SUB;

// EEPROM layout
const int     EEP_MAGIC_ADDR = 0;
const int     EEP_SAT_ADDR   = 1;
const int     EEP_SUB_ADDR   = 2;
const uint8_t EEP_MAGIC      = 0xA7;

// ---------- Bit Banger! ----------
// These two little functions, lo and hi, send the 0 or 1 bit respectively
static inline void lo(uint8_t p){ digitalWrite(p, LOW); }
static inline void hi(uint8_t p){ digitalWrite(p, HIGH); }

// shiftByteMSB creates the bit stream that sets volume using the hi & lo functions
void shiftByteMSB(uint8_t b){
  for (int i = 7; i >= 0; --i){
    lo(PIN_CLK);
    ((b >> i) & 1) ? hi(PIN_DATA) : lo(PIN_DATA);
    delayMicroseconds(1);
    hi(PIN_CLK);                 // rising edge clocks bit
    delayMicroseconds(1);
  }
  lo(PIN_CLK);
}

// lm1973_write uses the shiftByteMSB function to create commands, then sends it to chip
void lm1973_write(uint8_t addr, uint8_t data){
  lo(PIN_LOAD);
  delayMicroseconds(1);
  shiftByteMSB(addr);
  shiftByteMSB(data);
  hi(PIN_LOAD);
  delayMicroseconds(2);
  lo(PIN_LOAD);
  lo(PIN_DATA);
}

// ---------- Channel Selection Functions ----------
// These helper functions bundle settings to different addresses
void setSatellites(uint8_t data){
  lm1973_write(ADDR_CH1, data);
  lm1973_write(ADDR_CH2, data);
  lastSat = data;
}
void setSub(uint8_t data){
  lm1973_write(ADDR_CH3, data);
  lastSub = data;
}
void setBoth(uint8_t satData, uint8_t subData){
  setSatellites(satData);
  setSub(subData);
}

// Button Step Functions - Note: got lazy and used ChatGPT so these functions aren't human documented as well
static inline uint8_t clampAtt(uint8_t v){
  // Normalize any mute code to deepest attenuation before stepping
  if (v == CODE_MUTE_FF || v >= CODE_MUTE_1) v = CODE_DEEPEST;

  if (v < CODE_FULL_VOL)  v = CODE_FULL_VOL;
  if (v > CODE_DEEPEST)   v = CODE_DEEPEST;
  return v;
}

void stepSat(int8_t delta){
  uint8_t v = clampAtt(lastSat);

  // delta = -1 => reduce attenuation (louder)
  // delta = +1 => increase attenuation (quieter)
  int16_t nv = (int16_t)v + (int16_t)delta;
  if (nv < (int16_t)CODE_FULL_VOL) nv = CODE_FULL_VOL;
  if (nv > (int16_t)CODE_DEEPEST)  nv = CODE_DEEPEST;

  setSatellites((uint8_t)nv);
  saveSettings();

  Serial.print("BTN Sat -> 0x"); Serial.println(lastSat, HEX);
}

void stepSub(int8_t delta){
  uint8_t v = clampAtt(lastSub);

  int16_t nv = (int16_t)v + (int16_t)delta;
  if (nv < (int16_t)CODE_FULL_VOL) nv = CODE_FULL_VOL;
  if (nv > (int16_t)CODE_DEEPEST)  nv = CODE_DEEPEST;

  setSub((uint8_t)nv);
  saveSettings();

  Serial.print("BTN Sub -> 0x"); Serial.println(lastSub, HEX);
}

bool buttonPressed(ButtonEdge &b){
  bool reading = (digitalRead(b.pin) == HIGH);

  if (reading != b.lastReading){
    b.lastChangeMs = millis();
    b.lastReading = reading;
  }

  if ((millis() - b.lastChangeMs) > BTN_DEBOUNCE_MS){
    if (reading != b.stableState){
      b.stableState = reading;

      // We use INPUT_PULLUP; pressed = LOW => stableState becomes false
      if (b.stableState == false){
        return true; // falling edge (HIGH->LOW) == press
      }
    }
  }

  return false;
}

// Button state holders
ButtonEdge btnVUp   = { V_UP_PIN,   true, true, 0 };
ButtonEdge btnVDown = { V_DOWN_PIN, true, true, 0 };
ButtonEdge btnSUp   = { S_UP_PIN,   true, true, 0 };
ButtonEdge btnSDown = { S_DOWN_PIN, true, true, 0 };
// -----------------------------------------------------------------------------

// ---------- Mute Functions ----------
// muteChannelAllWays mutes then sets the system to lowest volume setting
void muteChannelAllWays(uint8_t addr){
  lm1973_write(addr, CODE_MUTE_1);
  lm1973_write(addr, CODE_MUTE_FF);
  lm1973_write(addr, CODE_DEEPEST);
}
// muteAll is self explanatory, using muteChannelAllWays
void muteAll(){
  muteChannelAllWays(ADDR_CH1);
  muteChannelAllWays(ADDR_CH2);
  muteChannelAllWays(ADDR_CH3);
  lastSat = CODE_MUTE_1;
  lastSub = CODE_MUTE_1;
}

// ---------- EEPROM - Volume Memory Management ----------
// Save and load settings using EEPROM library
void saveSettings(){
  EEPROM.update(EEP_MAGIC_ADDR, EEP_MAGIC);
  EEPROM.update(EEP_SAT_ADDR,   lastSat);
  EEPROM.update(EEP_SUB_ADDR,   lastSub);
}
void loadSettings(){
  if (EEPROM.read(EEP_MAGIC_ADDR) == EEP_MAGIC){
    lastSat = EEPROM.read(EEP_SAT_ADDR);
    lastSub = EEPROM.read(EEP_SUB_ADDR);
  } else {
    lastSat = DEFAULT_SAT;
    lastSub = DEFAULT_SUB;
    saveSettings();
  }
}

// ---------- Serial Control ----------

// handleCmd executes the inputed command string by executing the command and then 
// replaying the execution outcome to the serial monitor
void handleCmd(String s){
  s.trim(); s.toUpperCase();

  if (s == "MUTE"){ muteAll(); Serial.println("All -> MUTE"); return; }
  if (s == "FULL"){ setBoth(CODE_FULL_VOL, CODE_FULL_VOL); Serial.println("All -> 0x00"); return; }
  if (s == "SATFULL"){ setSatellites(CODE_FULL_VOL); Serial.println("Sat -> 0x00"); return; }
  if (s == "SUBFULL"){ setSub(CODE_FULL_VOL); Serial.println("Sub -> 0x00"); return; }
  if (s == "SAVE"){ saveSettings(); Serial.println("Saved."); return; }

  if (s.startsWith("SATL")){
    int n = s.substring(4).toInt();
    if (n>=0 && n<10){ setSatellites(ATT_TABLE[n]); Serial.print("Sat -> table["); Serial.print(n); Serial.println("]"); return; }
  }
  if (s.startsWith("SUBL")){
    int n = s.substring(4).toInt();
    if (n>=0 && n<10){ setSub(ATT_TABLE[n]); Serial.print("Sub -> table["); Serial.print(n); Serial.println("]"); return; }
  }
  if (s.startsWith("ALLL")){
    int n = s.substring(4).toInt();
    if (n>=0 && n<10){ setBoth(ATT_TABLE[n], ATT_TABLE[n]); Serial.print("All -> table["); Serial.print(n); Serial.println("]"); return; }
  }

  if (s.startsWith("SAT ")){
    uint8_t v = (uint8_t) strtoul(s.substring(4).c_str(), nullptr, 16);
    setSatellites(v); Serial.print("Sat -> 0x"); Serial.println(v, HEX); return;
  }
  if (s.startsWith("SUB ")){
    uint8_t v = (uint8_t) strtoul(s.substring(4).c_str(), nullptr, 16);
    setSub(v);        Serial.print("Sub -> 0x"); Serial.println(v, HEX); return;
  }
  if (s.startsWith("ALL ")){
    uint8_t v = (uint8_t) strtoul(s.substring(4).c_str(), nullptr, 16);
    setBoth(v, v);    Serial.print("All -> 0x"); Serial.println(v, HEX); return;
  }

  Serial.println("Cmds: MUTE | FULL | SATFULL | SUBFULL | SATL n | SUBL n | ALLL n | SAT xx | SUB xx | ALL xx | SAVE");
}

// ---------- Setup and Loop ----------
void setup(){
  // Set all microcontroller to LM1973M chip pins to output
  pinMode(PIN_CLK,  OUTPUT);
  pinMode(PIN_DATA, OUTPUT);
  pinMode(PIN_LOAD, OUTPUT);
  lo(PIN_CLK); lo(PIN_DATA); lo(PIN_LOAD);   // Initialize all to low/0

  // NEW: button inputs (normally-open to GND, use internal pullups)
  pinMode(V_UP_PIN,   INPUT_PULLUP);
  pinMode(V_DOWN_PIN, INPUT_PULLUP);
  pinMode(S_UP_PIN,   INPUT_PULLUP);
  pinMode(S_DOWN_PIN, INPUT_PULLUP);

  Serial.begin(9600);

  loadSettings();          // recall last
  delay(1200);             // rails/relays settle

  // Apply remembered settings to wake on power-up
  setBoth(lastSat, lastSub);

  saveSettings();          // ensure EEPROM has current values

  // Print curent volume on startup
  Serial.print("Boot applied: SAT=0x"); Serial.print(lastSat, HEX);
  Serial.print(" SUB=0x"); Serial.println(lastSub, HEX);
}

void loop(){
  // NEW: button handling (one step per press, debounced)
  if (buttonPressed(btnVUp))   { stepSat(-1); } // louder satellites
  if (buttonPressed(btnVDown)) { stepSat(+1); } // quieter satellites
  if (buttonPressed(btnSUp))   { stepSub(-1); } // louder sub
  if (buttonPressed(btnSDown)) { stepSub(+1); } // quieter sub

  if (Serial.available()){
    String line = Serial.readStringUntil('\n');
    handleCmd(line);
    saveSettings();        // auto-save after each command
  }

  // TA nudge required to sometimes "wake" the thing up
  static bool resent = false;
  if (!resent && millis() > 4000){
    setBoth(lastSat, lastSub);  // final wake resend
    resent = true;
  }
}