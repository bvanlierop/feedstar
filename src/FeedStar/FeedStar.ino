#include <LCD_I2C.h>
#include <DS3232RTC.h>      // https://github.com/JChristensen/DS3232RTC
#include <Streaming.h>      // https://github.com/janelia-arduino/Streaming
#include <Timemark.h>
#include <NRotary.h>
#include <Encoder.h>
#include <EEPROM.h>

enum BIN_STATE
{ 
  SHUT = 0,
  OPEN = 1
};

enum MENU_ACTIONS
{ 
  NOTHING = 0,
  SET_TIME = NOTHING + 1,
  RESET_SCHEMA = SET_TIME + 1
};

struct AlarmScheme {
  int number; // Bin number, etc.

  int hour;   // 0..24
  int minute; // 0..59

  bool isEditedHours;   // Indicator if the user is changing the hours
  bool isEditedMinutes; // Indicator if the user is changing the minutes
};

// INTERRUPT Wiring
// PIN D3 = brown (INT_RTC)
DS3232RTC myRTC;

// I2C Wiring
// A5 = blue (SDA)
// A4 = yellow (SDL)
LCD_I2C lcd(0x27, 20, 4);

// Pin definitions
constexpr uint8_t RTC_INT_PIN {2};  // RTC provides an alarm signal on this pin
const unsigned int SOFT_RESET_PIN = A0;
Timemark softResetButtonHold(2000);
bool _softResetButtonState = false;
const unsigned int DEMAGNETIZE_DELAY_MS = 2000;

// ROTARY Wiring --------------------------------
// D2 = yellow (KEY_Rotary)
const int _buttonPin = 5;
bool _buttonState;
const int DEBOUNCE_TIME_BUTTONS_MS = 40;
const int LONG_PRESS_BUTTON_TIME_MS = 500;
Timemark debounce(DEBOUNCE_TIME_BUTTONS_MS);
Timemark buttonHold(LONG_PRESS_BUTTON_TIME_MS);
//Rotary encoder pin A.
int rotaryAPin = 3;
//Rotary encoder pin B.
int rotaryBPin = 4; // Second rotary pin doesn't have to be an interrupt pin, still good performance
bool usingInterrupt = true;
Rotary rotary = Rotary(rotaryAPin, rotaryBPin, 12, usingInterrupt, INPUT_PULLUP, 50);
int rotaryState = IDLE;
bool rotarySwitch = false;
// ----------------------------------------------

const unsigned int NUMBER_OF_BINS = 7;

// RELAY Wiring (8 channel, we only use 7) ------
int _relayPins[NUMBER_OF_BINS] = { 6, 7, 8, 9, 10, 11, 12 };

// Status of all bins
BIN_STATE BINS[NUMBER_OF_BINS] = {SHUT, SHUT, SHUT, SHUT, SHUT, SHUT, SHUT};

// Default time: 20:00, 23:00, 02:00, 05:00, 8:00, 11:00, 14:00
AlarmScheme _as1 = { 1, 20, 0, false, false }; // Most bottom drawer
AlarmScheme _as2 = { 2, 20, 1, false, false };
AlarmScheme _as3 = { 3, 20, 2, false, false };
AlarmScheme _as4 = { 4, 20, 3, false, false };
AlarmScheme _as5 = { 5, 20, 4, false, false };
AlarmScheme _as6 = { 6, 20, 5, false, false };
AlarmScheme _as7 = { 7, 20, 6, false, false };

AlarmScheme *_alarmSchemes[NUMBER_OF_BINS] = { &_as1, &_as2, &_as3, &_as4, &_as5, &_as6, &_as7 };
bool _allowChangingTheHours = false;   // Controls the changing of hours
bool _allowChangingTheMinutes = false; // Controls the changing of minutes
int _currentBinIndex = 0; // When program boots, start with this bin / alarm
bool _testMode = false;
bool _menuIsActive = false;
int _currentSchemeSelection = 0;
bool _currentSchemeSubSelection = true; // True = Hour, False = Minute
MENU_ACTIONS _menuAction = 0;
bool _specialMenuActivated = false;
time_t _timeSnapshotForSpecialMenu = 0;

int _tempCurrentHour = -1;
int _tempCurrentMinute = -1;

void rotaryServiceRoutineWrapper() {
    rotary.serviceRoutine();
}

void setup() {
  Serial.begin(9600);

  // initialize the alarms to known values, clear the alarm flags, clear the alarm interrupt flags
  pinMode(RTC_INT_PIN, INPUT_PULLUP); // Set interrupt pin for RTC module (alarm signal)

  // ROTARY BUTTON INIT
  pinMode(_buttonPin, INPUT_PULLUP);
  _buttonState = digitalRead(_buttonPin);
  debounce.start();
  attachInterrupt(digitalPinToInterrupt(rotaryAPin), rotaryServiceRoutineWrapper, rotary.mode);

  // RELAY INIT
  pinMode(_relayPins[0], OUTPUT);
  pinMode(_relayPins[1], OUTPUT);
  pinMode(_relayPins[2], OUTPUT);
  pinMode(_relayPins[3], OUTPUT);
  pinMode(_relayPins[4], OUTPUT);
  pinMode(_relayPins[5], OUTPUT);
  pinMode(_relayPins[6], OUTPUT);

  // Send all LOW's to not turn on the magnets on boot (TODO)
  digitalWrite(_relayPins[0], HIGH);
  digitalWrite(_relayPins[1], HIGH);
  digitalWrite(_relayPins[2], HIGH);
  digitalWrite(_relayPins[3], HIGH);
  digitalWrite(_relayPins[4], HIGH);
  digitalWrite(_relayPins[5], HIGH);
  digitalWrite(_relayPins[6], HIGH);

  // Configure Soft Reset button on A0
  pinMode(SOFT_RESET_PIN, INPUT_PULLUP);
  _softResetButtonState = digitalRead(SOFT_RESET_PIN);

  // LCD INIT
  lcd.begin();
  lcd.backlight();
  
  // Init time module
  myRTC.begin();
  myRTC.setAlarm(DS3232RTC::ALM2_MATCH_DATE, 0, 0, 0, 1);
  myRTC.alarm(DS3232RTC::ALARM_2);
  myRTC.alarmInterrupt(DS3232RTC::ALARM_2, false);
  myRTC.squareWave(DS3232RTC::SQWAVE_NONE);

  // Configure the schema
  configureInitialSchema();

  // Setup Alarming
  configureAlarm();
}

void resetProgram() {

  // Re-initialize globals & menu items
  _allowChangingTheHours = false;
  _allowChangingTheMinutes = false;
  _currentBinIndex = 0;
  _testMode = false;
  _menuIsActive = false;
  _currentSchemeSelection = 0;
  _currentSchemeSubSelection = true;
  _menuAction = 0;
  _specialMenuActivated = false;
  _timeSnapshotForSpecialMenu = 0;
  _tempCurrentHour = -1;
  _tempCurrentMinute = -1;
  _softResetButtonState =false;

  // Reset bin states
  for(int i=0; i<sizeof(BINS) / sizeof(BIN_STATE); i++) {
    BINS[i] = SHUT;
  }
 
/* Set time: 
  tmElements_t tm;
  tm.Hour = 14;               // set the RTC time to 06:29:50
  tm.Minute = 37;
  tm.Second = 0;
  tm.Day = 6;
  tm.Month = 4;
  tm.Year = 2023 - 1970;      // tmElements_t.Year is the offset from 1970
  myRTC.write(tm);            // set the RTC from the tm structure
*/

  // Init time module
  myRTC.begin();
  myRTC.setAlarm(DS3232RTC::ALM2_MATCH_DATE, 0, 0, 0, 1);
  myRTC.alarm(DS3232RTC::ALARM_2);
  myRTC.alarmInterrupt(DS3232RTC::ALARM_2, false);
  myRTC.squareWave(DS3232RTC::SQWAVE_NONE);

  // Configure the schema
  configureInitialSchema();

  // Setup Alarming
  configureAlarm();
}

void configureAlarm() {
  int alarmHour = _alarmSchemes[0]->hour;
  int alarmMinute = _alarmSchemes[0]->minute;
  Serial << "Setting alarm #2 for first bin (ID: 0) on " << alarmHour << "h : " << alarmMinute << "m\n";
  myRTC.setAlarm(DS3232RTC::ALM2_MATCH_HOURS, 0, alarmMinute, alarmHour, 0);
  // clear the alarm flags
  myRTC.alarm(DS3232RTC::ALARM_2);
  // configure the INT/SQW pin for "interrupt" operation (i.e. disable square wave output)
  myRTC.squareWave(DS3232RTC::SQWAVE_NONE);
  // enable interrupt output for Alarm 2 only
  myRTC.alarmInterrupt(DS3232RTC::ALARM_2, true);
}

void configureInitialSchema() {
  // For testing, initiate alarms with quick testable schedule
  time_t t = myRTC.get();
  int currentHour = hour(t);
  int currentMinute = minute(t);
  AlarmScheme as1 = { 1, currentHour, currentMinute+1, false, false }; // Most bottom drawer
  AlarmScheme as2 = { 2, currentHour, currentMinute+2, false, false };
  AlarmScheme as3 = { 3, currentHour, currentMinute+3, false, false };
  AlarmScheme as4 = { 4, currentHour, currentMinute+4, false, false };
  AlarmScheme as5 = { 5, currentHour, currentMinute+5, false, false };
  AlarmScheme as6 = { 6, currentHour, currentMinute+6, false, false };
  AlarmScheme as7 = { 7, currentHour, currentMinute+7, false, false };

  for(int i=0; i<NUMBER_OF_BINS; i++) {
    _alarmSchemes[i]->hour = currentHour;
    _alarmSchemes[i]->minute = currentMinute + i + 1;
  }
}

void loop() {

  // Take care of soft resets by user long pressing a button
  handleSoftReset();

  // Handle test mode
  if(_testMode) {
    runTestMode();
    return;
  }

  // Read knob status
  readRotaryEncoderStates();

  // Handle button key presses (short & long press)
  handleButtonKeyPresses();

  // Check to see if the INT/SQW pin is low, i.e. an alarm has occurred
  handleAlarmOccurrence();

  // print the time when it changes
  static time_t tLast;
  time_t t = myRTC.get();
  if (t != tLast) {
    tLast = t;
    if(!_menuIsActive && !_specialMenuActivated) {
      drawStatusScreen(t);
    }
  }

  // Handle settings menu
  // Display menu (when button is pressed long enough)
  if(_menuIsActive && _menuAction == NOTHING) {
    showSettingsMenu();
  }

  // Show menu for changing the time
  if(_specialMenuActivated) {
    switch(_menuAction) {
      case NOTHING:
        _specialMenuActivated = false;
        break;
      case SET_TIME:
        lcd.setCursor(0, 0);
        lcd.print(F("Sys. tijd instellen:"));
        lcd.setCursor(0, 2);
        if(_timeSnapshotForSpecialMenu == 0) {
          _timeSnapshotForSpecialMenu = myRTC.get();
          _tempCurrentHour = hour(_timeSnapshotForSpecialMenu);
          _tempCurrentMinute = minute(_timeSnapshotForSpecialMenu);
        }
        
        lcdPrintDigits(_tempCurrentHour);
        lcd.print(":");
        lcdPrintDigits(_tempCurrentMinute);

        // Handle hours
        if (rotaryState == CLOCKWISE) {
          if(_tempCurrentHour < 23) {
            _tempCurrentHour++;
          } else {
            _tempCurrentHour = 0;
          }
        }
        if (rotaryState == COUNTER_CLOCKWISE) {
          if(_tempCurrentHour >= 1) {
            _tempCurrentHour--;
          } else {
            _tempCurrentHour = 23;
          }
        }

        break;
      case RESET_SCHEMA:
        _specialMenuActivated = true;
        break;
    }
  }
}

void handleSoftReset() {
  if (debounce.expired()) {
    bool currentResetState = digitalRead(SOFT_RESET_PIN);
    if (currentResetState != _softResetButtonState) {
      _softResetButtonState = currentResetState;
      if(currentResetState == LOW) {
        if(softResetButtonHold.expired()) {
          executeSoftReset();
        } else {
          Serial << "User pressed soft reset button but not long enough.\n";
        }
        softResetButtonHold.stop();
      } else {
        softResetButtonHold.start();
      }
    }
  }
}

void executeSoftReset() {
  Serial << "Executing soft reset ...\n";
  lcd.clear();
  lcd.println("RESETTING ...    ");
  resetProgram();
}

void readRotaryEncoderStates() {
  //Save the state of the rotary encoder. can only be called once per loop.
  rotaryState = rotary.getState();

  //Save the state of the rotary switch.
  rotarySwitch = rotary.getSwitch();

  //Returns true if not turning the rotary encoder. 
  if (rotaryState == IDLE) {
  }

  //Returns true if turning Counter-Clockwise. COUNTER_CLOCKWISE = 1.
  if (rotaryState == COUNTER_CLOCKWISE) {
    if(!_menuIsActive && !_specialMenuActivated) {
      int a = _menuAction;
      a--;
      if(a <= 0) {
        a = 0;
      }
      _menuAction = a;
      Serial << "cntr: " << a << "\n";
    }
  }

  //Returns true if turning Clockwise. CLOCKWISE = 2.
  if (rotaryState == CLOCKWISE) {
    if(!_menuIsActive && !_specialMenuActivated) {
      int a = _menuAction;
      a++;
      if(a >= 2) {
        a = 2;
      }
      _menuAction = a;
      Serial << "clk: " << a << "\n";
    }
  }
}

void handleButtonKeyPresses() {
  if (debounce.expired()) {
    bool currentState = digitalRead(_buttonPin);
    if (currentState != _buttonState) {
      _buttonState = currentState;
      if (_buttonState) {
        if (buttonHold.expired())
          doReturnButton();
        else
          doButton();
        buttonHold.stop();
      }
      else {
        buttonHold.start();
      }
    }
  }
}

void handleAlarmOccurrence() {
  if ( !digitalRead(RTC_INT_PIN) && (_currentBinIndex < NUMBER_OF_BINS) ) {
    myRTC.alarm(DS3232RTC::ALARM_2);    // reset the alarm flag
    Serial << "Alarm #2 has occurred!\n";

    if(_currentBinIndex < (sizeof(BINS) / sizeof(BIN_STATE))) {
      // Open first bin (from bottom to top)
      openBin(_currentBinIndex);
      _currentBinIndex++;

      if(_currentBinIndex == (sizeof(BINS) / sizeof(BIN_STATE))) {
        // When everything is empty, start again from the top (user has to make sure bins are manually closed & filled)
        BINS[7] = SHUT, SHUT, SHUT, SHUT, SHUT, SHUT, SHUT;
        _currentBinIndex = 0;
      }

      // Schedule alarm for next bin
      Serial.println(F("Setting new alarm ..."));
      Serial.print(F("Next bin (index) to open = "));
      Serial.print(_currentBinIndex);
      Serial.println();
      Serial.println(F("New alarm setting: "));
      int h = _alarmSchemes[_currentBinIndex]->hour;
      int m = _alarmSchemes[_currentBinIndex]->minute;
      Serial << h << ":" << m << "\n";
      Serial << "Setting alarm #2 bin (ID: " << _currentBinIndex << ") on " << h << "h : " << m << "m\n";
      myRTC.setAlarm(DS3232RTC::ALM2_MATCH_HOURS, 0, m, h, 0);
    }
  }
}

void drawStatusScreen(time_t t) {
  lcd.setCursor(0, 0);
  lcd.print(F("FSv0.1 "));
  lcdPrintDigits(hour(t));
  lcd.print(F(":"));
  lcdPrintDigits(minute(t));
  lcd.print(F(":"));
  lcdPrintDigits(second(t));
  lcd.print(F(" "));
  float celsius = myRTC.temperature() / 4.0;
  lcd.print((int)celsius);
  lcd.print((char)223);
  lcd.print(F("C"));

  lcd.setCursor(0, 1);
  lcd.print(F("Lades "));
  for(int i=0; i<sizeof(BINS) / sizeof(BIN_STATE); i++) {
    lcd.print(i+1); // One based in UI
    if(BINS[i] == SHUT) {
      lcd.print(F("-"));
    } else {
      lcd.print(F("|"));
    }
  }

  lcd.setCursor(0, 2);
  lcd.print(F("Voerbeurt > #"));
  lcd.print(_currentBinIndex+1);
  lcd.print(" ");
  lcdPrintDigits(_alarmSchemes[_currentBinIndex]->hour);
  lcd.print(F(":"));
  lcdPrintDigits(_alarmSchemes[_currentBinIndex]->minute);

  // Print special menu
  lcd.setCursor(0, 4);
  switch(_menuAction) {
    case NOTHING:
      lcd.print(F("             "));
    break;
    case SET_TIME:
      lcd.print(F("SET TIME     "));
    break;
    case RESET_SCHEMA:
      lcd.print(F("RESET SCHEMA "));
    break;
  }
}

bool isKnobRotatingClockwise() {
  return (rotaryState == CLOCKWISE);
}

bool isKnobRotatingCounterClockwise() {
  return (rotaryState == COUNTER_CLOCKWISE);
}

void handleSelection() {
  if(isKnobRotatingClockwise()) {
    selectNextEdit();
  }
  if(isKnobRotatingCounterClockwise()) {
    selectPreviousEdit();
  }
}

void selectNextEdit() {
  if(_allowChangingTheHours || _allowChangingTheMinutes) return; // Don't go to next edit when actually changing hours / minutes

  if(!_currentSchemeSubSelection) { // When on minute change, go to next scheme
   _currentSchemeSelection++;
  }

  _currentSchemeSubSelection = !_currentSchemeSubSelection;
  if(_currentSchemeSelection >= NUMBER_OF_BINS) _currentSchemeSelection = 0;
  //Serial << "Set alarm selection on: " << _currentSchemeSelection << "\n";
  setAlarmSchemeSelection(_currentSchemeSelection, _currentSchemeSubSelection);
}

void selectPreviousEdit() {
  if(_allowChangingTheHours || _allowChangingTheMinutes) return; // Don't go to next edit when actually changing hours / minutes

  if(!_currentSchemeSubSelection) { // When on minute change, go to next scheme
   _currentSchemeSelection--;
  }

  _currentSchemeSubSelection = !_currentSchemeSubSelection;
  if(_currentSchemeSelection < 0) _currentSchemeSelection = NUMBER_OF_BINS-1;
  //Serial << "Set alarm selection on: " << _currentSchemeSelection << "\n";
  setAlarmSchemeSelection(_currentSchemeSelection, _currentSchemeSubSelection);
}

void setAlarmSchemeSelection(int index, bool editHours) {
  if(index <= 0) index = 0;
  if(index >= NUMBER_OF_BINS) index = NUMBER_OF_BINS-1;

  for(int i=0; i<NUMBER_OF_BINS; i++) {
    _alarmSchemes[i]->isEditedHours = false;
    _alarmSchemes[i]->isEditedMinutes = false;
  }
  _alarmSchemes[index]->isEditedHours = editHours;
  _alarmSchemes[index]->isEditedMinutes = !editHours;
}

void drawAlarmSchemes() {
  handleSelection();
  lcd.setCursor(0, 0);
  drawAlarmSchemeOnLcd(_alarmSchemes[0]);
  lcd.setCursor(0, 1);
  drawAlarmSchemeOnLcd(_alarmSchemes[1]);
  lcd.setCursor(0, 2);
  drawAlarmSchemeOnLcd(_alarmSchemes[2]);
  lcd.setCursor(0, 3);
  drawAlarmSchemeOnLcd(_alarmSchemes[3]);
  lcd.setCursor(10, 0);
  drawAlarmSchemeOnLcd(_alarmSchemes[4]);
  lcd.setCursor(10, 1);
  drawAlarmSchemeOnLcd(_alarmSchemes[5]);
  lcd.setCursor(10, 2);
  drawAlarmSchemeOnLcd(_alarmSchemes[6]);
}

void drawAlarmSchemeOnLcd(AlarmScheme *as) {
  lcd.print(F("L"));
  lcd.print(as->number);
  if(as->isEditedHours) {
    lcd.print(F("["));
    lcdPrintDigits(as->hour);
    lcd.print(F("]"));
  } else {
    lcd.print(F(" "));
    lcdPrintDigits(as->hour);
  }
  if(as->isEditedMinutes) {
    lcd.print(F("["));
    lcdPrintDigits(as->minute);
    lcd.print(F("]"));
  } else {
    if(!as->isEditedHours) {
      lcd.print(F(":"));
    }
    lcdPrintDigits(as->minute);
    lcd.print(F(" "));
  }
}

void showSettingsMenu() {
  handleAlarmAdjustment();
  drawAlarmSchemes();
}

void handleAlarmAdjustment() {
  if(!_allowChangingTheHours && !_allowChangingTheMinutes) {
    // Don't adjust numbers when knob isn't pressed
    return;
  }

  if(isKnobRotatingClockwise()) {
    for(int i=0; i<NUMBER_OF_BINS; i++) {
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

  if(isKnobRotatingCounterClockwise()) {
    for(int i=0; i<NUMBER_OF_BINS; i++) {
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
}

void printDateTime(time_t t) {
    Serial << ((day(t)<10) ? "0" : "") << _DEC(day(t));
    Serial << monthShortStr(month(t)) << _DEC(year(t)) << ' ';
    Serial << ((hour(t)<10) ? "0" : "") << _DEC(hour(t)) << ':';
    Serial << ((minute(t)<10) ? "0" : "") << _DEC(minute(t)) << ':';
    Serial << ((second(t)<10) ? "0" : "") << _DEC(second(t));
}

void lcdPrintDigits(int digits) {
  // Pads digit with 0 when lower than 10
  if(digits < 10) {
    lcd.print(F("0"));
  }
  lcd.print(digits);
}

void doButton() {
  if(!_menuIsActive) {
    switch(_menuAction) {
      case NOTHING:
        _specialMenuActivated = false;
        break;
      case SET_TIME:
        //_specialMenuActivated = !_specialMenuActivated;
        //lcd.clear(); // Clear LCD for upcoming menu
        break;
      case RESET_SCHEMA:
        //_specialMenuActivated = !_specialMenuActivated;
        //lcd.clear(); // Clear LCD for upcoming menu
        break;
    }
  }

  // Handle logic for second button press
  if(_allowChangingTheHours || _allowChangingTheMinutes) {
    for(int i=0; i<NUMBER_OF_BINS; i++) {
      _alarmSchemes[i]->isEditedHours = false;
      _alarmSchemes[i]->isEditedMinutes = false;
    }
    _allowChangingTheHours = false;
    _allowChangingTheMinutes = false;
  }

  if(hoursAreHighlighted()) {
    // Allow the user to change the hours with the dial
    Serial << "Hours are highlighted\n";
    _allowChangingTheHours = true;
    _allowChangingTheMinutes = false;
  }

  if(minutesAreHighlighted()) {
    // Allow the user to change the minutes with the dial
    Serial << "Minutes are highlighted\n";
    _allowChangingTheHours = false;
    _allowChangingTheMinutes = true;
  }

  if(!_allowChangingTheHours && !_allowChangingTheMinutes) {
    // Store new alarm scheme in memory and EEPROM
    //saveAlarmSchemes();
    
    // After storing make sure the current alarms are reset
    resetAlarm(); 
  }
}

void resetAlarm() {
  Serial << "Resetting alarms ...\n";
  myRTC.clearAlarm(DS3232RTC::ALARM_2);
  // set Alarm 2
  int alarmHour = _alarmSchemes[_currentBinIndex]->hour;
  int alarmMinute = _alarmSchemes[_currentBinIndex]->minute;
  Serial << "Setting alarm #2 for bin " << _currentBinIndex+1 << " (ID: " << _currentBinIndex << ") on " << alarmHour << "h : " << alarmMinute << "m\n";
  myRTC.setAlarm(DS3232RTC::ALM2_MATCH_HOURS, 0, alarmMinute, alarmHour, 0);
}

bool hoursAreHighlighted() {
  for(int i=0; i<NUMBER_OF_BINS; i++) {
    if(_alarmSchemes[i]->isEditedHours) {
      return true;
    }
  }
  return false;
}

bool minutesAreHighlighted() {
  for(int i=0; i<NUMBER_OF_BINS; i++) {
    if(_alarmSchemes[i]->isEditedMinutes) {
      return true;
    }
  }
  return false;
}

void doReturnButton() {
  _menuIsActive = !_menuIsActive;
  lcd.clear();
  Serial.println(F("doReturnButton()"));
}

void openBin(unsigned int binNumber) {
  int currentPinForRelayInput = _relayPins[binNumber];
  BINS[binNumber] = OPEN;
  Serial << "Opening bin[" << binNumber << "] (#" << binNumber+1 << ") on relay IO pin: " << currentPinForRelayInput << " with demagnetize delay of: " << DEMAGNETIZE_DELAY_MS << " ms ...";
  toggleMagnet(currentPinForRelayInput);
}

void toggleMagnet(int pin) {
  digitalWrite(pin, LOW); // Why send LOW?
  delay(DEMAGNETIZE_DELAY_MS);
  digitalWrite(pin, HIGH); // Reset back to normal
}

void runTestMode() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("TEST MODE"));

  for(int i=0; i<NUMBER_OF_BINS; i++) {
    lcd.setCursor(0, 1);
    lcd.print(F("TEST RELAY PIN: "));
    lcd.print(_relayPins[i]);
    lcd.setCursor(0, 2);
    lcd.print(F("Magnet #"));
    lcd.print(i+1);
    lcd.print(F(" ["));
    lcd.print(i);
    lcd.print(F("]"));
    toggleMagnet(_relayPins[i]);
    delay(2500);
  }
  delay(5000);
}