// External libraries
#include <LCD_I2C.h>
#include <Streaming.h>
#include <IRremote.h>
#include <Timemark.h>
#include <DS3232RTC.h>

// Version info
#define VERSION_MAJOR 2
#define VERSION_MINOR 0

// Configure Arduino I/O mapping
#define RTC_INTERRUPT_PIN 2     // D2
#define IR_RECEIVE_PIN 3        // D3
#define BUTTON_LCD_BACKLIGHT 4  // D4

// Defines Serial communication parameters
#define SERIAL_BAUD_RATE 9600

// Defines LCD address and type
#define LCD_I2C_ADDRESS 0x27
#define LCD_COLUMNS_WIDTH 20
#define LCD_ROWS_HEIGHT 4

// Program definitions
#define LOOP_DELAY_MS 100  // Reduces LCD flicker and smoothens IR button response
#define PROGRAM_MODE_COOLDOWN_MS 5000
#define AMOUNT_OF_BINS_INSTALLED 7
#define DEMAGNETIZE_DELAY_MS 2000
#define MODE_SWITCH_REQUIRED_WAIT_MS 2000
#define DEBOUNCE_TIME_BUTTONS_MS 50
#define LONG_PRESS_DELAY_MS 4000
#define TIMEOUT_LCD_BACKLIGHT 120000

// Configure LCD module
LCD_I2C lcd(LCD_I2C_ADDRESS, LCD_COLUMNS_WIDTH, LCD_ROWS_HEIGHT);

// Configure Real time clock RTC for alarming (I2C address = 0x68; hardcoded)
DS3232RTC myRTC;

enum BIN_STATE {
  SHUT = 0,
  OPEN = 1
};

struct AlarmScheme {
  int number;  // Bin number, etc.

  int hour;    // 0..23
  int minute;  // 0..59

  bool isEditedHours;    // Indicator if the user is changing the hours
  bool isEditedMinutes;  // Indicator if the user is changing the minutes
};

// Program Globals

// Default time: 20:00, 23:00, 02:00, 05:00, 8:00, 11:00, 14:00
AlarmScheme _as1 = { 1, 20, 0, false, false };  // Most bottom drawer
AlarmScheme _as2 = { 2, 23, 0, false, false };
AlarmScheme _as3 = { 3, 2, 0, false, false };
AlarmScheme _as4 = { 4, 5, 0, false, false };
AlarmScheme _as5 = { 5, 8, 0, false, false };
AlarmScheme _as6 = { 6, 11, 0, false, false };
AlarmScheme _as7 = { 7, 14, 0, false, false };
BIN_STATE BINS[AMOUNT_OF_BINS_INSTALLED] = { SHUT, SHUT, SHUT, SHUT, SHUT, SHUT, SHUT };
AlarmScheme *_alarmSchemes[AMOUNT_OF_BINS_INSTALLED] = { &_as1, &_as2, &_as3, &_as4, &_as5, &_as6, &_as7 };

// Globals for controlling the program mode
// Null-terminated char arrays for buffering LCD text to reduce flicker issues
char _lcdLine0[21] = {0};
char _lcdLine1[21] = {0};
char _lcdLine2[21] = {0};
char _lcdLine3[21] = {0};
bool _manualOverrideIsActive = false;     // Allows user to control bins directly
bool _alarmEditorModeIsActive = false;    // Allows user to change alarm scheme
bool _systemClockEditorIsActive = false;  // Allows for changing the system clock
bool _sysClockUiSelectedHours = true;     // Shows indication for hours edit (default start with hours)
bool _sysClockUiSelectedMinutes = false;  // Shows indication for minutes edit
unsigned int _editingHoursValue = 8;      // User changes hours
unsigned int _editingMinutesValue = 0;    // User changes minutes
bool _allowChangingTheHours = false;      // Controls the changing of hours
bool _allowChangingTheMinutes = false;    // Controls the changing of minutes
unsigned int _currentSchemeSelection = 0; // User is editing this (custom) alarm scheme
bool _currentSchemeSubSelection = true;   // True = Hour, False = Minute
bool _openingBin = false;                 // Show opening in UI because of I/O magnet delay

// Timing variables
const unsigned long _interval = LONG_PRESS_DELAY_MS;
unsigned long _previousMillis;
unsigned long _previousMillisLcdBacklightTimeout;

// Globals for viewing and controlling the bins (magnets)
byte _manualBinSelection = 1;

// Relay pins
int _relayPins[AMOUNT_OF_BINS_INSTALLED] = { 6, 7, 8, 9, 10, 11, 12 };

// When program boots, start with this bin / magnet
int _currentBinIndex = 0;

// Mode switching requires delayed button press for accidental presses
Timemark debounce(DEBOUNCE_TIME_BUTTONS_MS);

void setup() {

  // Initialize Serial communication
  Serial.begin(SERIAL_BAUD_RATE);
  Serial << F("Starting FeedStar program ...\n");

  // Initialize LCD
  Serial << F("   > Initializing LCD (I2C=0x27)...\n");
  lcd.begin();
  lcd.backlight();
  lcd.clear();

  // Initialize IR remote
  Serial << F("   > Initializing IR remote ...\n");
  IrReceiver.begin(IR_RECEIVE_PIN, 0);
  debounce.start();  // IR button debouncing

  // Initialize time module
  Serial << F("   > Initializing RTC module (I2C=0x68) ...\n");
  pinMode(RTC_INTERRUPT_PIN, INPUT_PULLUP);  // Set interrupt pin for RTC module (alarm signal)

  // Initialize output pins for relay use
  pinMode(_relayPins[0], OUTPUT);
  pinMode(_relayPins[1], OUTPUT);
  pinMode(_relayPins[2], OUTPUT);
  pinMode(_relayPins[3], OUTPUT);
  pinMode(_relayPins[4], OUTPUT);
  pinMode(_relayPins[5], OUTPUT);
  pinMode(_relayPins[6], OUTPUT);

  // Don't turn on the magnets (release bins) during boot
  digitalWrite(_relayPins[0], HIGH);
  digitalWrite(_relayPins[1], HIGH);
  digitalWrite(_relayPins[2], HIGH);
  digitalWrite(_relayPins[3], HIGH);
  digitalWrite(_relayPins[4], HIGH);
  digitalWrite(_relayPins[5], HIGH);
  digitalWrite(_relayPins[6], HIGH);

  // Initialize LCD backlight button on front panel
  pinMode(BUTTON_LCD_BACKLIGHT, INPUT_PULLUP);

  // Immediately schedule first alarm (in case of reset or power dip)
  configureAlarm(0);

  _lcdLine0[20] = '\0';
  _lcdLine1[20] = '\0';
  _lcdLine2[20] = '\0';
  _lcdLine3[20] = '\0';
}

void configureAlarm(unsigned int firstAlarmIndex) {
  int alarmHour = _alarmSchemes[firstAlarmIndex]->hour;
  int alarmMinute = _alarmSchemes[firstAlarmIndex]->minute;
  Serial << F("Configuring alarm #2 for first bin (ID: 0) on ") << alarmHour << F("h : ") << alarmMinute << F("m ...\n");
  myRTC.begin();
  myRTC.setAlarm(DS3232RTC::ALM2_MATCH_HOURS, 0, alarmMinute, alarmHour, 0);
  // clear the alarm flags
  myRTC.alarm(DS3232RTC::ALARM_2);
  // configure the INT/SQW pin for "interrupt" operation (i.e. disable square wave output)
  myRTC.squareWave(DS3232RTC::SQWAVE_NONE);
  // enable interrupt output for Alarm 2 only
  myRTC.alarmInterrupt(DS3232RTC::ALARM_2, true);
  _currentBinIndex = firstAlarmIndex;
  time_t t = myRTC.get();
  int16_t temp = myRTC.temperature();
  float celsius = temp / 4.0;
  Serial << F("RTC hours is: ") << hour(t) << F("\n");
  Serial << F("RTC minutes is: ") << minute(t) << F("\n");
  Serial << F("RTC seconds is: ") << second(t) << F("\n");
  Serial << F("RTC temperature is: ") << celsius << F("\n");
}

void printBuffersToLcd() {
  lcd.setCursor(0, 0);
  lcd.print(_lcdLine0);
  lcd.setCursor(0, 1);
  lcd.print(_lcdLine1);
  lcd.setCursor(0, 2);
  lcd.print(_lcdLine2);
  lcd.setCursor(0, 3);
  lcd.print(_lcdLine3);
}

void loop() {

  // Write LCD via 4-line character buffers
  printBuffersToLcd();

  // Check alarm occurrence
  handleAlarmOccurrence();

  // Enable standby mode (LCD turns off)
  handleLcdBacklightStandbyMode();

  // Draw UI (only when there are no bins being opened and user is not editing time)
  if (!_openingBin) {
    // Edit system clock UI
    if(_systemClockEditorIsActive) {
      drawSystemClockEditorScreen();
    } else {
      time_t time = myRTC.get();
      int16_t temp = myRTC.temperature();
      drawTimeProgramModeAndTemp(time, temp);
      if (_manualOverrideIsActive) {
        drawManualSelectionScreen();
      } else {
        if (_alarmEditorModeIsActive) {
          drawAlarmEditorScreen();
        } else {
          drawBinStatusScreen();
        }
      }
    }
  }

  // Query possible IR remote button presses and act upon them
  pollIrRemote();

  // Delay to reduce LCD flicker and smoothen IR button response
  loopDelay();
}

void loopDelay() {
  const unsigned long microsInOneSecond=1000000UL;
  static unsigned long count, countStartMicros;
  count++;
  if (micros() - countStartMicros >= microsInOneSecond) // 1 second
  {
    countStartMicros += microsInOneSecond;
    count = 0;
  }
}

void handleAlarmOccurrence() {
  if(_systemClockEditorIsActive) {
    return;
  }

  if (!digitalRead(RTC_INTERRUPT_PIN) && (_currentBinIndex < AMOUNT_OF_BINS_INSTALLED)) {
    myRTC.alarm(DS3232RTC::ALARM_2);  // reset the alarm flag
    Serial << F("Alarm #2 has occurred!\n");

    if (_currentBinIndex < (sizeof(BINS) / sizeof(BIN_STATE))) {
      // Open first bin (from bottom to top)
      openBin(_currentBinIndex);
      _currentBinIndex++;

      if (_currentBinIndex >= AMOUNT_OF_BINS_INSTALLED-1) {
        // When everything is empty, start again from the top (user has to make sure bins are manually closed & filled)
        resetBins();
      }

      // Schedule alarm for next bin
      resetAlarm();
    }
  }
}

void resetBins() {
  BINS[0] = SHUT;
  BINS[1] = SHUT;
  BINS[2] = SHUT;
  BINS[3] = SHUT;
  BINS[4] = SHUT;
  BINS[5] = SHUT;
  BINS[6] = SHUT;
  _currentBinIndex = 0;
  Serial << F("Set all bins to shut and reset index to 0\n");
}

void drawSystemClockEditorScreen() {
  sprintf(_lcdLine0, "Sys. tijd instellen:");
  sprintf(_lcdLine1, "                    ");
  sprintf(_lcdLine2, "   %02d:%02d            ", 
    _editingHoursValue, _editingMinutesValue); 
  if(_sysClockUiSelectedHours) {
    sprintf(_lcdLine3, "    ^               ");
  } else {
    sprintf(_lcdLine3, "       ^            ");
  }
}

void setSystemClockTo(unsigned int hours, unsigned int minutes) {
  tmElements_t tm;
  tm.Hour = hours;
  tm.Minute = minutes;
  tm.Second = 0;
  myRTC.clearAlarm(DS3232RTC::ALARM_2); // Don't trigger alarm immediately when time is set back/forward
  myRTC.write(tm);
  Serial << F("Changed system time to to ") << hours << F(":") << minutes << F("\n");
  resetBins();
  resetAlarm();
}

void drawAlarmEditorScreen() {
  sprintf(_lcdLine0, "L%d %c%02d%c%02d  L%d %c%02d%c%02d", 
    _alarmSchemes[0]->number,
    ((_alarmSchemes[0]->isEditedHours) ? '>' : ' '),
    _alarmSchemes[0]->hour,
    ((_alarmSchemes[0]->isEditedMinutes) ? '>' : ':'),
    _alarmSchemes[0]->minute,
    _alarmSchemes[4]->number,
    ((_alarmSchemes[4]->isEditedHours) ? '>' : ' '),
    _alarmSchemes[4]->hour,
    ((_alarmSchemes[4]->isEditedMinutes) ? '>' : ':'),
    _alarmSchemes[4]->minute);
  
  sprintf(_lcdLine1, "L%d %c%02d%c%02d  L%d %c%02d%c%02d", 
    _alarmSchemes[1]->number,
    ((_alarmSchemes[1]->isEditedHours) ? '>' : ' '),
    _alarmSchemes[1]->hour,
    ((_alarmSchemes[1]->isEditedMinutes) ? '>' : ':'),
    _alarmSchemes[1]->minute,
    _alarmSchemes[5]->number,
    ((_alarmSchemes[5]->isEditedHours) ? '>' : ' '),
    _alarmSchemes[5]->hour,
    ((_alarmSchemes[5]->isEditedMinutes) ? '>' : ':'),
    _alarmSchemes[5]->minute);

  sprintf(_lcdLine2, "L%d %c%02d%c%02d  L%d %c%02d%c%02d", 
    _alarmSchemes[2]->number,
    ((_alarmSchemes[2]->isEditedHours) ? '>' : ' '),
    _alarmSchemes[2]->hour,
    ((_alarmSchemes[2]->isEditedMinutes) ? '>' : ':'),
    _alarmSchemes[2]->minute,
    _alarmSchemes[6]->number,
    ((_alarmSchemes[6]->isEditedHours) ? '>' : ' '),
    _alarmSchemes[6]->hour,
    ((_alarmSchemes[6]->isEditedMinutes) ? '>' : ':'),
    _alarmSchemes[6]->minute);

  sprintf(_lcdLine3, "L%d %c%02d%c%02d           ", 
    _alarmSchemes[3]->number,
    ((_alarmSchemes[3]->isEditedHours) ? '>' : ' '),
    _alarmSchemes[3]->hour,
    ((_alarmSchemes[3]->isEditedMinutes) ? '>' : ':'),
    _alarmSchemes[3]->minute);
}

void selectNextEdit() {
  if(!_currentSchemeSubSelection) { // When on minute change, go to next scheme
   _currentSchemeSelection++;
  }

  _currentSchemeSubSelection = !_currentSchemeSubSelection;
  _allowChangingTheHours = !_allowChangingTheHours;
  _allowChangingTheMinutes = !_allowChangingTheMinutes;
  if(_currentSchemeSelection >= AMOUNT_OF_BINS_INSTALLED) {
     _currentSchemeSelection = 0;
  }
  Serial << F("Set alarm selection on: ") << _currentSchemeSelection << F("\n");
  setAlarmSchemeSelection(_currentSchemeSelection, _currentSchemeSubSelection);
}

void selectPreviousEdit() {
  if(!_currentSchemeSubSelection) { // When on minute change, go to next scheme
   _currentSchemeSelection--;
  }

  _currentSchemeSubSelection = !_currentSchemeSubSelection;
  _allowChangingTheHours = !_allowChangingTheHours;
  _allowChangingTheMinutes = !_allowChangingTheMinutes;
  if(_currentSchemeSelection < 0) _currentSchemeSelection = AMOUNT_OF_BINS_INSTALLED-1;
  Serial << F("Set alarm selection on: ") << _currentSchemeSelection << F("\n");
  setAlarmSchemeSelection(_currentSchemeSelection, _currentSchemeSubSelection);
}

void incrementHoursAndMinutesOfAlarmScheme() {
  for(int i=0; i<AMOUNT_OF_BINS_INSTALLED; i++) {
    if(_alarmSchemes[i]->isEditedHours && _allowChangingTheHours) {
      if(_alarmSchemes[i]->hour < 23) {
        _alarmSchemes[i]->hour++;
      } else {
        _alarmSchemes[i]->hour = 0;
      }
    }
    if(_alarmSchemes[i]->isEditedMinutes && _allowChangingTheMinutes) {
      if(_alarmSchemes[i]->minute < 59) {
        _alarmSchemes[i]->minute++;
      } else {
        _alarmSchemes[i]->minute = 0;
      }
    }
  }
}

void decrementHoursAndMinutesOfAlarmScheme() {
  for(int i=0; i<AMOUNT_OF_BINS_INSTALLED; i++) {
    if(_alarmSchemes[i]->isEditedHours && _allowChangingTheHours) {
      if(_alarmSchemes[i]->hour >= 1) {
        _alarmSchemes[i]->hour--;
      } else {
        _alarmSchemes[i]->hour = 23;
      }
    }
    if(_alarmSchemes[i]->isEditedMinutes && _allowChangingTheMinutes) {
      if(_alarmSchemes[i]->minute >= 1) {
        _alarmSchemes[i]->minute--;
      } else {
        _alarmSchemes[i]->minute = 59;
      }
    }
  }
}

void setAlarmSchemeSelection(int index, bool editHours) {
  if(index <= 0) index = 0;
  if(index >= AMOUNT_OF_BINS_INSTALLED) index = AMOUNT_OF_BINS_INSTALLED-1;

  for(int i=0; i<AMOUNT_OF_BINS_INSTALLED; i++) {
    _alarmSchemes[i]->isEditedHours = false;
    _alarmSchemes[i]->isEditedMinutes = false;
  }
  _alarmSchemes[index]->isEditedHours = editHours;
  _alarmSchemes[index]->isEditedMinutes = !editHours;
}

void handleLcdBacklightStandbyMode() {
  unsigned long currentMillis = millis();
  if (currentMillis - _previousMillisLcdBacklightTimeout > TIMEOUT_LCD_BACKLIGHT) {
    _previousMillisLcdBacklightTimeout = currentMillis;

    // Turn off lcd
    Serial << F("Turning off backlight due to inactivity ...\n");
    lcd.noBacklight();
  }
}

void drawTimeProgramModeAndTemp(time_t time, int16_t temp) {
  int hours = hour(time);
  int minutes = minute(time);
  int seconds = second(time);
  float celsius = temp / 4.0;
  if(_manualOverrideIsActive) {
    sprintf(_lcdLine0, "%02d:%02d:%02d *HAND*%d %cC", hours, minutes, seconds, (int)celsius, (char)223);
  } else {
    sprintf(_lcdLine0, "%02d:%02d:%02d       %d %cC", hours, minutes, seconds, (int)celsius, (char)223);
  }
}

void drawManualSelectionScreen() {
  sprintf(_lcdLine0, "Kies lade: <,>,OK   ");
  sprintf(_lcdLine1, "                    ");
  sprintf(_lcdLine2, "%cL%d%c %cL%d%c %cL%d%c %cL%d%c ", 
  displayStartEditSymbol(1, _manualBinSelection), 1, displayEndEditSymbol(1, _manualBinSelection),
  displayStartEditSymbol(2, _manualBinSelection), 2, displayEndEditSymbol(2, _manualBinSelection),
  displayStartEditSymbol(3, _manualBinSelection), 3, displayEndEditSymbol(3, _manualBinSelection),
  displayStartEditSymbol(4, _manualBinSelection), 4, displayEndEditSymbol(4, _manualBinSelection));
  sprintf(_lcdLine3, "%cL%d%c %cL%d%c %cL%d%c      ", 
  displayStartEditSymbol(5, _manualBinSelection), 5, displayEndEditSymbol(5, _manualBinSelection),
  displayStartEditSymbol(6, _manualBinSelection), 6, displayEndEditSymbol(6, _manualBinSelection),
  displayStartEditSymbol(7, _manualBinSelection), 7, displayEndEditSymbol(7, _manualBinSelection));
}

char displayStartEditSymbol(unsigned int a, unsigned int b){
  if(a == b) {
    return '[';
  } else {
    return ' ';
  }
}

char displayEndEditSymbol(unsigned int a, unsigned int b){
  if(a == b) {
    return ']';
  } else {
    return ' ';
  }
}

bool binIsSelectedManually(unsigned int binIndex) {
  return (_manualBinSelection == binIndex);
}

void drawBinStatusScreen() {
  sprintf(_lcdLine1, "Voerschema:   %c%02d:%02d", 
  getPrefixSymbol(_alarmSchemes[0]->number), _alarmSchemes[0]->hour, _alarmSchemes[0]->minute);
  sprintf(_lcdLine2, "%c%02d:%02d %c%02d:%02d %c%02d:%02d", 
  getPrefixSymbol(_alarmSchemes[1]->number), _alarmSchemes[1]->hour, _alarmSchemes[1]->minute, 
  getPrefixSymbol(_alarmSchemes[2]->number), _alarmSchemes[2]->hour, _alarmSchemes[2]->minute, 
  getPrefixSymbol(_alarmSchemes[3]->number), _alarmSchemes[3]->hour, _alarmSchemes[3]->minute);
  sprintf(_lcdLine3, "%c%02d:%02d %c%02d:%02d %c%02d:%02d", 
  getPrefixSymbol(_alarmSchemes[4]->number), _alarmSchemes[4]->hour, _alarmSchemes[4]->minute, 
  getPrefixSymbol(_alarmSchemes[5]->number), _alarmSchemes[5]->hour, _alarmSchemes[5]->minute, 
  getPrefixSymbol(_alarmSchemes[6]->number), _alarmSchemes[6]->hour, _alarmSchemes[6]->minute);
}

char getPrefixSymbol(int number) {
  char prefixSymbol = '-'; // Default is shut
  if(number == _currentBinIndex+1) {
    // Show mark on next bin that is going to be opened, one based
    prefixSymbol = '>';
  } else if (BINS[number-1] == OPEN) {
    prefixSymbol = '|';
  }
  return prefixSymbol;
}

void drawFullScreenBinOpeningScreen(unsigned int bin) {
  sprintf(_lcdLine0, "                   ");
  sprintf(_lcdLine1, "   Opening: L%d     ", bin);
  sprintf(_lcdLine2, "                   ");
  sprintf(_lcdLine3, "                   ");
}

void openBin(unsigned int binNumber) {
  if(_systemClockEditorIsActive) {
    Serial << F("openBin() is requested but skipping because user is editing system clock ...");
    return;
  }

  _openingBin = true;
  drawFullScreenBinOpeningScreen(binNumber + 1);  // UI is one based
  int currentPinForRelayInput = _relayPins[binNumber];
  BINS[binNumber] = OPEN;
  Serial << F("Opening bin[") << binNumber << F("] (#") << binNumber + 1 << F(") on relay IO pin: ") << currentPinForRelayInput << F(" with demagnetize delay of: ") << DEMAGNETIZE_DELAY_MS << F(" ms ...\n");
  toggleMagnet(currentPinForRelayInput);
  _openingBin = false;
}

void toggleMagnet(int pin) {
  digitalWrite(pin, LOW);
  delay(DEMAGNETIZE_DELAY_MS);
  digitalWrite(pin, HIGH);
}

void showVersionInfo() {
  sprintf(_lcdLine0, "FeedStar V%d.%d    ", VERSION_MAJOR, VERSION_MINOR);
  sprintf(_lcdLine1, "                   ");
  sprintf(_lcdLine2, "                   ");
  sprintf(_lcdLine3, "                   ");
}

void resetAlarm() {
  // Schedule alarm for next bin
  Serial << F("Setting new alarm ...\n");
  Serial << F("Next bin (index) to open = ") << _currentBinIndex << F("\n");
  int h = _alarmSchemes[_currentBinIndex]->hour;
  int m = _alarmSchemes[_currentBinIndex]->minute;
  Serial << F("New alarm setting: ") << h << F(":") << m << F("\n");
  Serial << F("Setting alarm #2 bin (ID: ") << _currentBinIndex << F(") on ") << h << F("h : ") << m << F("m\n");
  myRTC.setAlarm(DS3232RTC::ALM2_MATCH_HOURS, 0, m, h, 0);
}

void insertTestAlarmScheme() {
  // Sets whole scheme to test mode (now() + 1min per bin difference)
  time_t time = myRTC.get();
  unsigned int minuteValue = minute(time);
  unsigned int hourValue = hour(time);
  if(minuteValue >= 52) { // Guard that we don't exceed minute addition because we have 7 bins
    minuteValue = 0;
    hourValue++;
    if(hourValue > 23) {
      hourValue = 0;
    }
  }
  _alarmSchemes[0]->hour = hourValue;
  _alarmSchemes[1]->hour = hourValue;
  _alarmSchemes[2]->hour = hourValue;
  _alarmSchemes[3]->hour = hourValue;
  _alarmSchemes[4]->hour = hourValue;
  _alarmSchemes[5]->hour = hourValue;
  _alarmSchemes[6]->hour = hourValue;
  _alarmSchemes[0]->minute = minuteValue+2;
  _alarmSchemes[1]->minute = minuteValue+3;
  _alarmSchemes[2]->minute = minuteValue+4;
  _alarmSchemes[3]->minute = minuteValue+5;
  _alarmSchemes[4]->minute = minuteValue+6;
  _alarmSchemes[5]->minute = minuteValue+7;
  _alarmSchemes[6]->minute = minuteValue+8;
  // Configure the first one to get started
  configureAlarm(0);
  Serial << F("Test Scheme active (every bin +1m from now)") << F("\n");
}

bool hoursAreHighlighted() {
  for(int i=0; i<AMOUNT_OF_BINS_INSTALLED; i++) {
    if(_alarmSchemes[i]->isEditedHours) {
      return true;
    }
  }
  return false;
}

bool minutesAreHighlighted() {
  for(int i=0; i<AMOUNT_OF_BINS_INSTALLED; i++) {
    if(_alarmSchemes[i]->isEditedMinutes) {
      return true;
    }
  }
  return false;
}

void handleAlarmSchemeEditing() {
  // Save current editing value (hours or minutes)
  Serial << F("Save requested, next edit.\n");
  if(_allowChangingTheHours || _allowChangingTheMinutes) {
    for(int i=0; i<AMOUNT_OF_BINS_INSTALLED; i++) {
      _alarmSchemes[i]->isEditedHours = false;
      _alarmSchemes[i]->isEditedMinutes = false;
    }
    _allowChangingTheHours = false;
    _allowChangingTheMinutes = false;
  }

  if(hoursAreHighlighted()) {
    // Allow the user to change the hours
    Serial << F("Hours are highlighted\n");
    _allowChangingTheHours = true;
    _allowChangingTheMinutes = false;
  }

  if(minutesAreHighlighted()) {
    // Allow the user to change the minutes
    Serial << F("Minutes are highlighted\n");
    _allowChangingTheHours = false;
    _allowChangingTheMinutes = true;
  }

  if(!_allowChangingTheHours && !_allowChangingTheMinutes) {
    // After storing make sure the current alarms are reset
    resetAlarm(); 
  }
}

void incrementSystemClockHoursAndMinutes() {
  if(_sysClockUiSelectedHours) {
    if(_editingHoursValue >= 23)
      _editingHoursValue = 0;
    else
      _editingHoursValue++;
    Serial << F("Changed sys clock hours to: ") << _editingHoursValue << F("\n");
  } else {
    if(_editingMinutesValue >= 59)
      _editingMinutesValue = 0;
    else
      _editingMinutesValue++;
    Serial << F("Changed sys clock minutes to: ") << _editingMinutesValue << F("\n");
  }
}

void decrementSystemClockHoursAndMinutes() {
  if(_sysClockUiSelectedHours) {
    if(_editingHoursValue <= 0)
      _editingHoursValue = 23;
    else
      _editingHoursValue--;
    Serial << F("Changed sys clock hours to: ") << _editingHoursValue << F("\n");
  } else {
    if(_editingMinutesValue <= 0)
      _editingMinutesValue = 59;
    else
      _editingMinutesValue--;
    Serial << F("Changed sys clock minutes to: ") << _editingMinutesValue << F("\n");
  }
}

void pollIrRemote() {
  unsigned long currentMillis = millis();
  if (IrReceiver.decode()) {
    uint16_t command = IrReceiver.decodedIRData.command;
    if (command != 0) {
      if (!debounce.expired()) return;
      // Handle supported buttons
      switch (command) {
        case 5:  // CH+
          Serial.println("UP");
          lcd.backlight();
          if(_systemClockEditorIsActive) {
            incrementSystemClockHoursAndMinutes();
          } else if(_alarmEditorModeIsActive) {
            incrementHoursAndMinutesOfAlarmScheme();
          }
          break;
        case 2:  // CH-
          Serial.println("DOWN");
          lcd.backlight();
          if(_systemClockEditorIsActive) {
            decrementSystemClockHoursAndMinutes();
          } else if(_alarmEditorModeIsActive) {
            decrementHoursAndMinutesOfAlarmScheme();
          }
          break;
        case 10:  // VOL-
          Serial.println("LEFT");
          lcd.backlight();
          if(_systemClockEditorIsActive) {
            _sysClockUiSelectedHours = !_sysClockUiSelectedHours;
            _sysClockUiSelectedMinutes = !_sysClockUiSelectedMinutes;
            Serial << F("System clock hour/minute is toggled! State hours = ") << _sysClockUiSelectedHours << F("\n"); 
            Serial << F("System clock hour/minute is toggled! State minutes = ") << _sysClockUiSelectedMinutes << F("\n"); 
          } else if (_manualOverrideIsActive) {
            if (_manualBinSelection <= 1) {  // _manualBinSelection is 1-based
              _manualBinSelection = AMOUNT_OF_BINS_INSTALLED;
            } else {
              _manualBinSelection--;
            }
            Serial << F("Manual bin selection: ") << _manualBinSelection << F("\n");
          } else if(_alarmEditorModeIsActive) {
            selectPreviousEdit();
          }
          break;
        case 30:  // VOL+
          Serial.println("RIGHT");
          lcd.backlight();
          if(_systemClockEditorIsActive) {
            _sysClockUiSelectedHours = !_sysClockUiSelectedHours;
            _sysClockUiSelectedMinutes = !_sysClockUiSelectedMinutes;
            Serial << F("System clock hour/minute is toggled! State hours = ") << _sysClockUiSelectedHours << F("\n"); 
            Serial << F("System clock hour/minute is toggled! State minutes = ") << _sysClockUiSelectedMinutes << F("\n");           
          } else if (_manualOverrideIsActive) {
            if (_manualBinSelection >= AMOUNT_OF_BINS_INSTALLED) {
              _manualBinSelection = 1;
            } else {
              _manualBinSelection++;
            }
            Serial << F("Manual bin selection: ") << _manualBinSelection << F("\n");
          } else if(_alarmEditorModeIsActive) {
            selectNextEdit();
          }
          break;
        case 64:  // FULLSCREEN
          Serial.println("ENTER");
          lcd.backlight();
          if(_systemClockEditorIsActive) {
            setSystemClockTo(_editingHoursValue, _editingMinutesValue);
            _systemClockEditorIsActive = false;
          } else {
            if (_manualOverrideIsActive) {
              openBin(_manualBinSelection - 1);  // bins are zero based
            }
          }
          break;
        case 28:  // RECALL
          Serial.println("ESC");
          lcd.backlight();
          // Check if user was editing the custom alarm schedule
          if(_alarmEditorModeIsActive) {
            // Save any changes
            // Start with editing hours of first alarm (L1)
            _allowChangingTheHours = true;
            setAlarmSchemeSelection(0, _allowChangingTheHours);
            configureAlarm(0);
            Serial << F("Custom scheme active!\n");
          }
          _manualOverrideIsActive = false;
          _alarmEditorModeIsActive = false;
          _systemClockEditorIsActive = false;
          break;
        case 22:  // MUTE (green)
          Serial.println("MODE");
          lcd.backlight();
          if ((currentMillis - _previousMillis >= _interval)) {
            _previousMillis = currentMillis;
            _manualOverrideIsActive = !_manualOverrideIsActive;
            _alarmEditorModeIsActive = false;
            _systemClockEditorIsActive = false;
            Serial << F("Manual override is toggled! State = ") << _manualOverrideIsActive << F("\n");
          }
          break;
        case 12:  // TIMESHIFT
          Serial.println("TIMESHIFT");
          lcd.backlight();
          Serial << F("User requested alarm scheme editor UI ...\n");
          _alarmEditorModeIsActive = true;
          _manualOverrideIsActive = false;
          _systemClockEditorIsActive = false;

          // Start with editing hours of first alarm (L1)
          Serial << F("Allow changing the hours on first alarm scheme ...\n");
          _allowChangingTheHours = true;
          setAlarmSchemeSelection(0, _allowChangingTheHours);
          break;
        case 76:  // RECORD
          Serial.println("RECORD");
          lcd.backlight();
          _alarmEditorModeIsActive = false;
          _manualOverrideIsActive = false;
          _systemClockEditorIsActive = true;
          Serial << F("User requested to edit system clock ...\n");
          break;
        case 77:  // POWER
          Serial.println("POWER");
          lcd.backlight();
          break;
        case 84:  // SOURCE
          Serial.println("SOURCE");
          lcd.backlight();
          if(_alarmEditorModeIsActive) {
            insertTestAlarmScheme();  
          } else {
            showVersionInfo();
          }
          break;
        default:
          Serial.println(command);
          lcd.backlight();
      }
    }
    IrReceiver.resume();
  }
}