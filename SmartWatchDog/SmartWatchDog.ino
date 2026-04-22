// Davide Achilli - 25-MAR-2026
//
// SmartWatchDog v1.0
//
// Project for ATMega328 (Arduino Nano, Uno, etc.)
//
// 2020-03-31 v1.2  On ALIVE, the relay is unpowered
//
#include <EEPROM.h>
const int LED_PIN = 13;
const int RELAY_PIN = A5;

// This macro enables the use of the ATMega32 internal watchdog. This watchdog
// resets this board in case *it* gets stuck.
// Essentially, it becomes the watchdog of the watchdog.
// Unfortunately, the “old” Arduino Nano bootloader (the one programmed at 57600 bps)
// has a bug: if the watchdog resets it, the board never restarts and the LED on PIN 13
// blinks rapidly.
// It works only on boards updated with the new bootloader programmed at 115200 bps.
#define USE_ATMEGA32_WATCHDOG 1

#if USE_ATMEGA32_WATCHDOG
#include <avr/wdt.h> /* Header for watchdog timers in AVR */
#endif


#define INCLUDE_DEBUG_COMMANDS 1

// Minimum and maximum boot timeout. Values outside these limits will not be accepted
#define TIMEOUT_BOOT_MIN     ( 1 * 3600)    // Minimum: 1 hour
#define TIMEOUT_BOOT_MAX     (48 * 3600)    // Maximum: 2 days
#define TIMEOUT_BOOT_DEFAULT ( 6 * 3600)    // Default: 6 hours

// Minimum and maximum ALIVE timeout. Values outside these limits will not be accepted
#define TIMEOUT_ALIVE_MIN     (60)        // Minimum: 1 minute
#define TIMEOUT_ALIVE_MAX     (48 * 3600) // Maximum: 2 days
#define TIMEOUT_ALIVE_DEFAULT (30 * 60)   // Default: 30 minutes

// Minimum and maximum values for the power‑off period required for reboot.
#define TIME_OFF_MIN     (1)         // Minimum: 1 second
#define TIME_OFF_MAX     (10 * 60)   // Maximum: 10 minutes
#define TIME_OFF_DEFAULT (4)         // Default: 4 seconds

#define HASH_INFO "#INFO "

// Random signature. Used to detect whether this EEPROM has been programmed by this
// application or contains leftover data from other programs.
#define EEPROM_SIGNATURE 0xABC918E1
#define EEPROM_SIGNATURE_ADDRESS 0x00

#define EEPROM_DEFAULTS_ADDRESS 0x04

#define COMMAND_BUFFER_SIZE 32
char commandBuffer[COMMAND_BUFFER_SIZE];
uint8_t commandBufferOffset = 0;
bool lastWasSeparator = false;
bool badCommand = false;

#define COMMAND_PARAMETERS_COUNT 4
uint8_t commandParameter[COMMAND_PARAMETERS_COUNT];
uint8_t commandParameterCount = 0;

static const char VERSION[] PROGMEM = "1.2";

static const char HELP_TEXT[] PROGMEM = 
  "HELP:\n"
  "VER           show the version\n"
  "ALIVE         inform the watchdog that the client is alive\n"
  "STATUS        see current timeout limits and status\n"
  "SETBOOT <s>   set the boot timeout to the indicated number of seconds <s>\n"
  "SETALIVE <s>  set the alive timeout to the indicated number of seconds <s>\n"
  "SETOFF <s>    set the off time to the indicated number of seconds <s>\n"
  "REBOOT <s>    trigger a reboot after the indicated number of seconds <s>; send ALIVE to cancel\n"
  "DISABLE       disable the watchdog\n"
  "ENABLE        enable the watchdog\n"
  #if INCLUDE_DEBUG_COMMANDS
  "ON            power the relay (debug)\n"
  "OFF           unpower the relay (debug)\n"
  "DEBUGADD <s>  add <s> seconds to current time (debug)\n"
  "DEBUGSUB <s>  subtract <s> seconds from current time (debug)\n"
  #if USE_ATMEGA32_WATCHDOG
  "NOWD          disable the internal watchdog reset; used to test activation of the ATMega328 watchdog"
  #endif // USE_ATMEGA32_WATCHDOG
  #endif // INCLUDE_DEBUG_COMMANDS
;

// Contains the boot timestamp before an ALIVE is received.
// Therefore it contains the timestamp of the last ALIVE.
// In case of reboot, it contains the moment when the reboot was requested.
uint32_t wdLastEventSeconds = 0;

uint32_t getElapsed() {return nowInSeconds() - wdLastEventSeconds;}

// Current state
enum class WDState {
  INITIAL_BOOT,     // Just started but never received an ALIVE
  ALIVE_RECEIVED,   // Received at least one ALIVE
  EXECUTING_OFF,    // Currently powering off
  REBOOT_REQUESTED, // A reboot has been explicitly requested
};
WDState wdState;

uint32_t wdRebootRequestTime = 0;

void ok()
{
  Serial.println(F("OK"));
}

// Power or unpower the relay
bool relay(bool activate)
{
  uint8_t action = (activate ? HIGH : LOW);
  bool changed = (action != digitalRead(RELAY_PIN));
  digitalWrite(LED_PIN, action);
  digitalWrite(RELAY_PIN, action);
  return changed;
}

#if INCLUDE_DEBUG_COMMANDS
int32_t debugAdditionalSeconds = 0;
#endif // INCLUDE_DEBUG_COMMANDS



// Ritorna i secondi correnti (partendo da uno zero arbitrario)
inline uint32_t nowInSeconds() {
  uint32_t now = millis()/1000;

  #if INCLUDE_DEBUG_COMMANDS
  now += debugAdditionalSeconds;
  #endif // INCLUDE_DEBUG_COMMANDS

  return now;
}

//-------------------------------------------------------------
// WATCHDOG MANAGEMENT
//-------------------------------------------------------------
struct WDLimits {
  // Set to true if the watchdog is enabled.
  bool enabled;

	// Boot wait time. If after an Arduino and/or system reboot
	// no "ALIVE" messages arrive, it will wait this amount of time
	// before rebooting again.
	// The time must be very long because the machine responsible for sending ALIVE
	// messages might be undergoing an update, and we do not want to reboot it during that period.
  uint32_t initialBootTimeoutSeconds;

	// Waiting time after receiving an ALIVE before performing the reboot.
	// ALIVE messages must be sent at a much higher frequency for safety.
	// Allow enough time so that the ALIVE‑sending program can be stopped
	// for maintenance without causing continuous reboots.
  uint32_t aliveTimeoutSeconds;

	// Number of seconds the power supply is kept off so that all capacitors can discharge
	// and the reset can be considered complete.
  uint32_t offTimeSeconds;

  // Set values within the max ranges
  bool setWithinRange() {
    bool rearranged = false;
    if (initialBootTimeoutSeconds < TIMEOUT_BOOT_MIN) {rearranged=true; initialBootTimeoutSeconds=TIMEOUT_BOOT_MIN;}
    if (initialBootTimeoutSeconds > TIMEOUT_BOOT_MAX) {rearranged=true; initialBootTimeoutSeconds=TIMEOUT_BOOT_MAX;}
    if (aliveTimeoutSeconds < TIMEOUT_ALIVE_MIN) {rearranged=true; aliveTimeoutSeconds=TIMEOUT_ALIVE_MIN;}
    if (aliveTimeoutSeconds > TIMEOUT_ALIVE_MAX) {rearranged=true; aliveTimeoutSeconds=TIMEOUT_ALIVE_MAX;}
    if (offTimeSeconds < TIME_OFF_MIN) {rearranged=true; offTimeSeconds=TIME_OFF_MIN;}
    if (offTimeSeconds > TIME_OFF_MAX) {rearranged=true; offTimeSeconds=TIME_OFF_MAX;}
    return rearranged;
  }

  // Set defaults
  void setDefaults() {
    initialBootTimeoutSeconds = TIMEOUT_BOOT_DEFAULT;
    aliveTimeoutSeconds = TIMEOUT_ALIVE_DEFAULT;
    offTimeSeconds = TIME_OFF_DEFAULT;
    setWithinRange();
  }

  // Read from EEPROM
  void readFromEEPROM() {
    uint8_t* ptr = (uint8_t*)this;
    for (unsigned int i = 0; i < sizeof(*this); i++) {
      ptr[i] = EEPROM.read(EEPROM_DEFAULTS_ADDRESS + i);
    }    
    setWithinRange();
  }

  // Write to EEPROM
  void writeToEEPROM() const {
    const uint8_t* ptr = (const uint8_t*)this;
    for (unsigned int i = 0; i < sizeof(*this); i++) {
      EEPROM.update(EEPROM_DEFAULTS_ADDRESS + i, ptr[i]);
    }    
  }

  // Print current data
  void printData() const {
    Serial.print(enabled ? F("ENABLED") : F("DISABLED"));
    Serial.print(F(" BOOT="));
    Serial.print(initialBootTimeoutSeconds);
    Serial.print(F(" ALIVE="));
    Serial.print(aliveTimeoutSeconds);
    Serial.print(F(" OFF="));
    Serial.print(offTimeSeconds);
    Serial.print(F(" STATUS="));
    switch(wdState) {
      case WDState::INITIAL_BOOT: Serial.print(F("INITIAL_BOOT")); break;
      case WDState::ALIVE_RECEIVED: Serial.print(F("ALIVE_RECEIVED")); break;
      case WDState::EXECUTING_OFF: Serial.print(F("EXECUTING_OFF")); break;
      case WDState::REBOOT_REQUESTED: 
        Serial.print(F("REBOOT_REQUESTED(")); 
        Serial.print(wdRebootRequestTime);
        Serial.print(F(")")); 
        break;
      default: Serial.print(F("UNKNOWN"));
    }
    Serial.print(F(" ELAPSED="));
    Serial.print(getElapsed());
    Serial.print(F(" RELAY="));
    if (digitalRead(RELAY_PIN) == HIGH) Serial.print(F("POWERED"));
    else Serial.print(F("UNPOWERED"));
    Serial.println();
  }
};

WDLimits wdLimits;

void wdInit()
{
  wdLastEventSeconds = nowInSeconds();
  wdState = WDState::INITIAL_BOOT;
}

// Starts the reboot procedure
void wdStartReboot()
{
  wdLastEventSeconds = nowInSeconds();
  wdState = WDState::EXECUTING_OFF;
  relay(true); // Activates the relay, which will cut the power
}

// Analyzes the current situation based on the state
void wdLoop()
{
  uint32_t elapsed = getElapsed();

  // Performs different actions depending on the current state
  switch(wdState) {
      case WDState::INITIAL_BOOT:
        // If the time elapsed since boot exceeds the configured limit, perform another reboot
        if (wdLimits.enabled && (elapsed > wdLimits.initialBootTimeoutSeconds)) {
          Serial.println(F("Boot time exceeded: rebooting"));
          wdStartReboot();
        }
        break;
      case WDState::ALIVE_RECEIVED:
        // If the time elapsed since the last ALIVE exceeds the configured limit, perform another reboot
        if (wdLimits.enabled && (elapsed > wdLimits.aliveTimeoutSeconds)) {
          Serial.println(F("Alive time exceeded: rebooting"));
          wdStartReboot();
        }
        break;
      case WDState::EXECUTING_OFF:
        // If the power‑off time has elapsed, restart the system
        if (elapsed > wdLimits.offTimeSeconds) {
          Serial.println(F("Off time ended. Turning on"));
          relay(false);
          wdLastEventSeconds = nowInSeconds();
          wdState = WDState::INITIAL_BOOT;
        }
        break;
      case WDState::REBOOT_REQUESTED:
        // If the time elapsed since the reboot request has passed, execute it
        if (elapsed > wdRebootRequestTime) {
          Serial.println(F("Forced reboot wait time expired: rebooting"));
          wdStartReboot();
        }
        break;
  }
}


void wdSetEEPROMDefaults()
{
  wdLimits.setDefaults();
  wdLimits.writeToEEPROM();
}

void wdLoadEEPROM()
{
  wdLimits.readFromEEPROM();
}

void wdAlive()
{
  switch(wdState) {
      case WDState::INITIAL_BOOT:
        Serial.println(F("Received first ALIVE after boot"));
        break;
      case WDState::ALIVE_RECEIVED:
        break;
      case WDState::EXECUTING_OFF:
        Serial.println(F("Unexpected: received ALIVE while off"));
        break;
      case WDState::REBOOT_REQUESTED:
        Serial.println(F("Forced reboot cancelled"));
        break;
  }

  wdLimits.printData();
  relay(false);
  wdLastEventSeconds = nowInSeconds();
  wdState = WDState::ALIVE_RECEIVED;
}

//-------------------------------------------------------------

bool paramToU32(uint8_t paramNum, uint32_t& value)
{
  if (paramNum >= commandParameterCount) return false;
  uint32_t prev=0;
  value=0;
  const char* p = commandBuffer+commandParameter[paramNum];
  while ((*p) != 0) {
    // Character not between zero and nine
    if (((*p) < '0') || ((*p) > '9')) return false;

    value *= 10;
    value += (uint32_t)((*p) - '0');
    // If the new value is smaller than the previous one, overflow occurred.
    // In other words, the number entered is too large.
    if (value < prev) {
      return false;
    }
    prev = value;
    
    p++;
  }

  return true;
}

// Clears the current command and resets everything to the initial state
void clearCommand()
{
  commandBufferOffset = 0;
  commandParameterCount = 0;
  badCommand = false;
  lastWasSeparator = false;
}

#define DISPLAY_COMMAND_DEBUG 0

#if DISPLAY_COMMAND_DEBUG
void displayCommandForDebug()
{
  if (badCommand) {
    Serial.println(F("BAD COMMAND"));
    return;
  }

  Serial.print(F("COMMAND=["));
  Serial.print(commandBuffer);
  Serial.print(F("]"));
  for (uint8_t i=0; i<commandParameterCount; i++) {
    Serial.print(F(" P=["));
    Serial.print(commandBuffer+commandParameter[i]);
    Serial.print(F("]"));
  }
  Serial.println();
}
#endif

bool checkCommand(const char* command, uint8_t expectedParams)
{
  if (strcmp(commandBuffer, command) != 0) return false;
  if (commandParameterCount != expectedParams) {
    Serial.print(F("Bad number of parameters; type 'HELP' for help."));
    return false;
  }
  return true;
}

void setSeconds(uint32_t& target)
{
  uint32_t seconds = 0;
  if (!paramToU32(0, seconds)) {
    Serial.println(F("ERROR: expected number of seconds"));
    return;
  }
  target = seconds;
  if (wdLimits.setWithinRange()) {
    Serial.print(F("WARNING: value was out of range; set to "));
    Serial.println(target);
  }
  else {
    ok();
  }
  wdLimits.writeToEEPROM();
}


void setBootTimeout()
{
  setSeconds(wdLimits.initialBootTimeoutSeconds);
}

void setAliveTimeout()
{
  setSeconds(wdLimits.aliveTimeoutSeconds);
}

void setOffTime()
{
  setSeconds(wdLimits.offTimeSeconds);
}

void forceReboot()
{
  uint32_t seconds = 0;
  if (!paramToU32(0, seconds)) {
    Serial.println(F("ERROR: expected number of seconds"));
    return;
  }
  wdRebootRequestTime = seconds;
  wdLastEventSeconds = nowInSeconds();
  wdState = WDState::REBOOT_REQUESTED;
  ok();
}

#if INCLUDE_DEBUG_COMMANDS
void debugAddSub(bool add)
{
  uint32_t seconds = 0;
  if (!paramToU32(0, seconds)) {
    Serial.println(F("ERROR: expected number of seconds"));
    return;
  }
  if (add) debugAdditionalSeconds += seconds;
  else debugAdditionalSeconds -= seconds;
  ok();
}

bool internalWatchdogRearmingDisabled = false;
void disableInternalWatchdogRearming()
{
  internalWatchdogRearmingDisabled = true;
  ok();
}

#endif // INCLUDE_DEBUG_COMMANDS

void enableWatchDog()
{
  if (wdLimits.enabled) {
    Serial.println(F("ALREADY ENABLED"));
  }
  else {
    Serial.println(F("ENABLED"));
    wdLimits.enabled = true; 
    wdLimits.writeToEEPROM();
    wdInit();
  }
}

void disableWatchDog()
{
  if (!wdLimits.enabled) {
    Serial.println(F("ALREADY DISABLED"));
  }
  else {
    Serial.println(F("DISABLED")); 
    wdLimits.enabled = false; 
    wdLimits.writeToEEPROM();
  }
}


void executeCommand()
{
  if (checkCommand("HELP", 0)) Serial.println((const __FlashStringHelper*)HELP_TEXT);
  else if (checkCommand("VER", 0)) Serial.println((const __FlashStringHelper*)VERSION);
  else if (checkCommand("ALIVE", 0)) wdAlive();
  else if (checkCommand("STATUS", 0)) wdLimits.printData();
  else if (checkCommand("SETBOOT", 1)) setBootTimeout();
  else if (checkCommand("SETALIVE", 1)) setAliveTimeout();
  else if (checkCommand("SETOFF", 1)) setOffTime();
  else if (checkCommand("REBOOT", 1)) forceReboot();
  else if (checkCommand("ENABLE", 0)) enableWatchDog();
  else if (checkCommand("DISABLE", 0)) disableWatchDog();

  #if INCLUDE_DEBUG_COMMANDS
  else if (checkCommand("ON", 0)) {relay(true); ok();}
  else if (checkCommand("OFF", 0)) {relay(false); ok();}
  else if (checkCommand("DEBUGADD", 1)) debugAddSub(true);
  else if (checkCommand("DEBUGSUB", 1)) debugAddSub(false);
  #if USE_ATMEGA32_WATCHDOG
  else if (checkCommand("NOWD", 0)) disableInternalWatchdogRearming();
  #endif // USE_ATMEGA32_WATCHDOG
  #endif // INCLUDE_DEBUG_COMMANDS

  else {
    Serial.println(F("Command unknown; type 'HELP' for help."));
  }
  Serial.println(F("#EOL"));
}

// This function reads characters from the serial port and splits them
// into “tokens”, i.e., parts that can be processed.
void checkSerial()
{
  while (Serial.available() > 0) {
    byte b = Serial.read();
    // If a newline is found, execute the command
    if (b == '\n') {
      commandBuffer[commandBufferOffset] = 0;
      #if DISPLAY_COMMAND_DEBUG
      displayCommandForDebug();
      #endif
      executeCommand();
      clearCommand();
    }
    // Proceed only if there is space left in the internal structures. Otherwise discard everything.
    else if ((commandBufferOffset < (COMMAND_BUFFER_SIZE-2)) && (commandParameterCount < COMMAND_PARAMETERS_COUNT)) {
      // Convert everything to uppercase
      if ((b >= 'a') && (b <= 'z')) b = b - 'a' + 'A';

      // Detect spaces and prepare for the next parameter
      if (b == ' ') {
        if (commandBufferOffset > 0) lastWasSeparator = true;
      }
      else {
        // Allow only text and numbers
        if ((b>='A' && b <= 'Z') || (b>='0' && b <= '9')) {
          if (lastWasSeparator) {
            commandBuffer[commandBufferOffset] = '\0';
            commandBufferOffset++;
            commandParameter[commandParameterCount] = commandBufferOffset;
            commandParameterCount++;
            lastWasSeparator = false;
          }
          commandBuffer[commandBufferOffset] = (char)b;
          commandBufferOffset++;
        }
        else badCommand = true;
      }
    }
    else badCommand = true;
  }

}

void eepromUpdateUint32(int address, uint32_t value) {
  for (int i = 0; i < 4; i++) {
    byte b = (value >> (8 * i)) & 0xFF;
    EEPROM.update(address + i, b);
  }
}


void initEEPROM()
{
  uint32_t signature;
  EEPROM.get(EEPROM_SIGNATURE_ADDRESS, signature);
  if (signature == EEPROM_SIGNATURE) {
    Serial.println(F(HASH_INFO "EEPROM SIGNATURE VALID"));
    wdLoadEEPROM();
  }
  else {
    Serial.println(F(HASH_INFO "EEPROM SIGNATURE INVALID: SET DEFAULT VALUES"));
    eepromUpdateUint32(EEPROM_SIGNATURE_ADDRESS, EEPROM_SIGNATURE);
    wdSetEEPROMDefaults();
  }
}

void setup()
{
  #if USE_ATMEGA32_WATCHDOG
  wdt_disable();  /* Disable the watchdog and wait for more than 2 seconds */
  #endif
  Serial.begin(115200);
  Serial.println(F(HASH_INFO "INIT"));
  clearCommand();
  initEEPROM();
  wdInit();

  memset(commandBuffer, 0, sizeof(commandBuffer));
  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // Blink the LED 3 times to indicate reboot
  for (int i=0; i<3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(300);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }

  #if USE_ATMEGA32_WATCHDOG
  wdt_enable(WDTO_8S);
  #endif
}

void loop()
{
  checkSerial();
  wdLoop();
  #if USE_ATMEGA32_WATCHDOG
  #if INCLUDE_DEBUG_COMMANDS
  if (!internalWatchdogRearmingDisabled) {
  #endif // INCLUDE_DEBUG_COMMANDS
    wdt_reset();
  #if INCLUDE_DEBUG_COMMANDS
  }
  #endif // INCLUDE_DEBUG_COMMANDS
  #endif // USE_ATMEGA32_WATCHDOG
}
