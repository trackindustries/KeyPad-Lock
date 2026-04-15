#include <TM1637Display.h>
#include <Keypad.h>

// | Keypad Pin | ESP32   |
// | ---------- | ------- |
// | R1         | GPIO 13 |
// | R2         | GPIO 12 |
// | R3         | GPIO 14 |
// | R4         | GPIO 27 |
// | C1         | GPIO 26 |
// | C2         | GPIO 33 |
// | C3         | GPIO 25 |


// | TM1637 Pin | ESP32      |
// | ---------- | ---------- |
// | CLK        | GPIO 19    |
// | DIO        | GPIO 18    |
// | VCC        | 3.3V or 5V |
// | GND        | GND        |


// ── Pin Definitions ───────────────────────────────────────────
#define CLK_PIN  19
#define DIO_PIN  18
#define LOCK_PIN 32

// ── Keypad Layout ─────────────────────────────────────────────
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS]  = {26, 33, 25};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
TM1637Display display(CLK_PIN, DIO_PIN);

// ── Segment Definitions ───────────────────────────────────────
const uint8_t DIGIT_SEG[10] = {
  0b0111111, // 0
  0b0000110, // 1
  0b1011011, // 2
  0b1001111, // 3
  0b1100110, // 4
  0b1101101, // 5
  0b1111101, // 6
  0b0000111, // 7
  0b1111111, // 8
  0b1101111  // 9
};

// Spinning animation segments: A B C D E F
const uint8_t SPIN_SEG[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};

#define DASH 0x40  // Middle segment only (dash)

// ── State Machine ─────────────────────────────────────────────
enum State { NORMAL, SET_VERIFY_OLD, SET_ENTER_NEW, SET_CONFIRM_NEW };
State currentState = NORMAL;

char passcode[5]      = "1234";
char entryBuffer[5]   = "";
char newCodeBuffer[5] = "";
int  entryPos         = 0;

// ── Timing ────────────────────────────────────────────────────
unsigned long lastFlash   = 0;
bool          colonState  = false;

// ── Star/Hash set-mode trigger ────────────────────────────────
unsigned long starPressTime = 0;
bool          starPending   = false;

// ── Secret hold state ─────────────────────────────────────────
unsigned long starHoldStart = 0;
bool          starIsHeld    = false;  // physically held right now
bool          starHeld5     = false;  // completed 5-second hold
bool          watchingHash  = false;
unsigned long hashHoldStart = 0;
bool          hashIsHeld    = false;  // physically held right now

// ── Helpers ───────────────────────────────────────────────────
void blankDisplay() {
  display.clear();
}

// Show digits built from entryBuffer
void showEntry() {
  uint8_t buf[4] = {0, 0, 0, 0};
  for (int i = 0; i < entryPos; i++)
    buf[i] = DIGIT_SEG[entryBuffer[i] - '0'];
  display.setSegments(buf);
}

// Show digits with colon on
void showEntryWithColon() {
  uint8_t buf[4];
  for (int i = 0; i < 4; i++)
    buf[i] = (i < entryPos) ? DIGIT_SEG[entryBuffer[i] - '0'] : 0x00;
  buf[1] |= 0x80;
  display.setSegments(buf);
}

// Flash colon on/off every 1 second
void flashColon() {
  if (millis() - lastFlash >= 1000) {
    lastFlash  = millis();
    colonState = !colonState;
    if (colonState) {
      uint8_t buf[4] = {0x00, 0x80, 0x00, 0x00};
      display.setSegments(buf);
    } else {
      blankDisplay();
    }
  }
}

// Steady colon, no digits
void steadyColon() {
  uint8_t buf[4] = {0x00, 0x80, 0x00, 0x00};
  display.setSegments(buf);
}

void resetEntry() {
  memset(entryBuffer, 0, sizeof(entryBuffer));
  entryPos = 0;
}

void enterSetMode() {
  currentState = SET_VERIFY_OLD;
  resetEntry();
  lastFlash  = millis();
  colonState = false;
  blankDisplay();
}

void abortToNormal() {
  currentState = NORMAL;
  resetEntry();
  starIsHeld   = false;
  starHeld5    = false;
  watchingHash = false;
  hashIsHeld   = false;
  starPending  = false;
  blankDisplay();
}

// ── Spin animation on correct code ───────────────────────────
void spinAnimation() {
  int frameDelay = 2000 / (3 * 6);
  for (int rev = 0; rev < 3; rev++) {
    for (int seg = 0; seg < 6; seg++) {
      uint8_t buf[4];
      for (int d = 0; d < 4; d++) buf[d] = SPIN_SEG[seg];
      display.setSegments(buf);
      delay(frameDelay);
      blankDisplay();
      delay(frameDelay / 4);
    }
  }
  blankDisplay();
}

// ── Flash new code 3x ─────────────────────────────────────────
void flashNewCode(const char* code) {
  uint8_t buf[4];
  for (int i = 0; i < 4; i++) buf[i] = DIGIT_SEG[code[i] - '0'];
  for (int i = 0; i < 3; i++) {
    display.setSegments(buf);
    delay(500);
    blankDisplay();
    delay(500);
  }
}

// ── Reveal PIN ────────────────────────────────────────────────
void revealPIN() {
  uint8_t buf[4];
  for (int i = 0; i < 4; i++) buf[i] = DIGIT_SEG[passcode[i] - '0'];
  for (int i = 0; i < 3; i++) {
    display.setSegments(buf);
    delay(700);
    blankDisplay();
    delay(300);
  }
}

// ── Secret hold: dashes fill/drain while keys are physically held ─
void handleSecretHold() {
  // Phase 1: * is being physically held — fill dashes
  if (starIsHeld && !starHeld5) {
    unsigned long held = millis() - starHoldStart;
    int dashCount = min((int)(held / 1000) + 1, 4);
    uint8_t buf[4] = {0, 0, 0, 0};
    for (int d = 0; d < dashCount; d++) buf[d] = DASH;
    display.setSegments(buf);
    if (held >= 5000) {
      starHeld5    = true;
      watchingHash = true;
      uint8_t full[4] = {DASH, DASH, DASH, DASH};
      display.setSegments(full);
    }
  }

  // Phase 2: # is being physically held — drain dashes
  if (hashIsHeld && watchingHash) {
    unsigned long held = millis() - hashHoldStart;
    int dashCount = 4 - min((int)(held / 1000), 4);
    uint8_t buf[4] = {0, 0, 0, 0};
    for (int d = 0; d < dashCount; d++) buf[d] = DASH;
    display.setSegments(buf);
    if (held >= 5000) {
      starIsHeld   = false;
      starHeld5    = false;
      hashIsHeld   = false;
      watchingHash = false;
      blankDisplay();
      delay(300);
      revealPIN();
    }
  }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  display.setBrightness(7);
  blankDisplay();
  pinMode(LOCK_PIN, OUTPUT);
  digitalWrite(LOCK_PIN, LOW);
  keypad.setDebounceTime(50);
  // Enable hold events — key must be held for 500 ms to fire KEY_HOLD
  keypad.setHoldTime(500);
}

// ── Main Loop ─────────────────────────────────────────────────
void loop() {

  if (currentState == NORMAL) handleSecretHold();
  if (currentState == SET_VERIFY_OLD) flashColon();
  if (starPending && (millis() - starPressTime > 1500)) starPending = false;

  // ── Scan all keys for PRESSED / HOLD / RELEASED events ──────
  if (keypad.getKeys()) {
    for (int i = 0; i < LIST_MAX; i++) {
      if (!keypad.key[i].stateChanged) continue;

      char     key   = keypad.key[i].kchar;
      KeyState state = keypad.key[i].kstate;

      // ── * key ──────────────────────────────────────────────
      if (key == '*') {
        if (state == PRESSED) {
          if (currentState == NORMAL) {
            // Start tracking a potential hold; also flag a quick-press
            // for the *+# set-mode shortcut
            starIsHeld    = true;
            starHoldStart = millis();
            starHeld5     = false;
            watchingHash  = false;
            blankDisplay();
            starPending   = true;
            starPressTime = millis();
          } else {
            abortToNormal();
          }
        }
        else if (state == RELEASED) {
          // Key released before completing hold — cancel everything
          if (!starHeld5) {
            starIsHeld = false;
            if (!starPending) {
              // Released too late for quick-press shortcut, not held long
              // enough for secret — just clear display
              blankDisplay();
            }
            // If starPending is still true the user may still press # quickly
          }
          // If starHeld5 is true, keep watching for # — do NOT clear
        }
        continue;
      }

      // ── # key ──────────────────────────────────────────────
      if (key == '#') {
        if (state == PRESSED) {
          if (currentState == NORMAL && starPending && !watchingHash) {
            // Quick *+# → enter set mode
            starPending   = false;
            starPressTime = 0;
            starIsHeld    = false;
            starHeld5     = false;
            enterSetMode();
          } else if (currentState == NORMAL && watchingHash && !hashIsHeld) {
            // Begin the # hold phase
            hashIsHeld    = true;
            hashHoldStart = millis();
          } else if (currentState != NORMAL) {
            abortToNormal();
          }
        }
        else if (state == RELEASED) {
          // # released before completing 5-second hold — abort secret
          if (hashIsHeld && watchingHash) {
            hashIsHeld   = false;
            starHeld5    = false;
            watchingHash = false;
            starIsHeld   = false;
            blankDisplay();
          }
        }
        continue;
      }

      // ── Digit keys (PRESSED only) ───────────────────────────
      if (state != PRESSED) continue;

      // Any digit cancels star hold
      starPending  = false;
      starIsHeld   = false;
      starHeld5    = false;
      watchingHash = false;
      hashIsHeld   = false;

      // ── NORMAL ───────────────────────────────────────────────
      if (currentState == NORMAL) {
        if (key >= '0' && key <= '9' && entryPos < 4) {
          entryBuffer[entryPos++] = key;
          showEntry();
          if (entryPos == 4) {
            entryBuffer[4] = '\0';
            delay(300);
            if (strcmp(entryBuffer, passcode) == 0) {
              spinAnimation();
              digitalWrite(LOCK_PIN, HIGH);
              delay(2000);
              digitalWrite(LOCK_PIN, LOW);
            }
            resetEntry();
            blankDisplay();
          }
        }
      }

      // ── SET_VERIFY_OLD ────────────────────────────────────────
      else if (currentState == SET_VERIFY_OLD) {
        if (key >= '0' && key <= '9' && entryPos < 4) {
          entryBuffer[entryPos++] = key;
          showEntryWithColon();
          if (entryPos == 4) {
            entryBuffer[4] = '\0';
            delay(300);
            if (strcmp(entryBuffer, passcode) == 0) {
              steadyColon();
              currentState = SET_ENTER_NEW;
              resetEntry();
            } else {
              abortToNormal();
            }
          }
        }
      }

      // ── SET_ENTER_NEW ─────────────────────────────────────────
      else if (currentState == SET_ENTER_NEW) {
        if (key >= '0' && key <= '9' && entryPos < 4) {
          entryBuffer[entryPos++] = key;
          showEntryWithColon();
          if (entryPos == 4) {
            entryBuffer[4] = '\0';
            memcpy(newCodeBuffer, entryBuffer, 5);
            delay(300);
            currentState = SET_CONFIRM_NEW;
            resetEntry();
            steadyColon();
          }
        }
      }

      // ── SET_CONFIRM_NEW ───────────────────────────────────────
      else if (currentState == SET_CONFIRM_NEW) {
        if (key >= '0' && key <= '9' && entryPos < 4) {
          entryBuffer[entryPos++] = key;
          showEntryWithColon();
          if (entryPos == 4) {
            entryBuffer[4] = '\0';
            delay(300);
            if (strcmp(entryBuffer, newCodeBuffer) == 0) {
              memcpy(passcode, newCodeBuffer, 5);
              flashNewCode(passcode);
            }
            abortToNormal();
          }
        }
      }

    } // end key loop
  } // end getKeys()
}
