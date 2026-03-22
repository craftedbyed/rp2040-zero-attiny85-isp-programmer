/**********************************   Minimal RP2040 ISP     *************************************

* Based on the original Arduino ISP sketch by Randall Bohn
* Maintains avrdude compatibility and original ArduinoISP protocol
* Modifications for RP2040:
      -- Custom bitbanged SPI pins
      -- SPI on GP2...4, RESET on GP5
      -- RGB LED status indication using NeoPixel
* WS2812 status LED on GP16: READY = Blue, PROGRAMMING = Yellow, SUCCESS = GREEN, ERROR = Rred
* Code modified and refined by Edmond Francis for Waveshare's RP2040 Zero on Mar 22, 2026

*************************************************************************************************/

#include <Adafruit_NeoPixel.h>

#define MOSI_PIN  3   // RP2040 GP3  -> ATtiny85 PB0 (Pin 5) MOSI
#define MISO_PIN  4   // RP2040 GP4  -> ATtiny85 PB1 (Pin 6) MISO
#define SCK_PIN   2   // RP2040 GP2  -> ATtiny85 PB2 (Pin 7) SCK
#define RESET_PIN 5   // RP2040 GP5  -> ATtiny85 PB5 (Pin 1) RESET
#define LED_PIN  16   // WS2812 built-in on Waveshare RP2040 Zero
#define BAUDRATE 19200

// STK constants
#define STK_OK 0x10
#define STK_FAILED 0x11
#define STK_UNKNOWN 0x12
#define STK_INSYNC 0x14
#define STK_NOSYNC 0x15
#define CRC_EOP 0x20

Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

// Minimal BitBanged SPI
class BitBangedSPI {
public:
  void begin() {
    digitalWrite(SCK_PIN, LOW);
    digitalWrite(MOSI_PIN, LOW);
    pinMode(SCK_PIN, OUTPUT);
    pinMode(MOSI_PIN, OUTPUT);
    pinMode(MISO_PIN, INPUT);
  }

  void beginTransaction(uint32_t clock) {
    // compute approximate half-period in microseconds
    pulse = max(1u, (unsigned long)((500000UL + clock - 1) / clock));
  }

  uint8_t transfer(uint8_t b) {
    for (int i = 0; i < 8; ++i) {
      digitalWrite(MOSI_PIN, (b & 0x80) ? HIGH : LOW);
      digitalWrite(SCK_PIN, HIGH);
      delayMicroseconds(pulse);
      b = (b << 1) | digitalRead(MISO_PIN);
      digitalWrite(SCK_PIN, LOW);
      delayMicroseconds(pulse);
    }
    return b;
  }

private:
  unsigned long pulse = 2;
};

static BitBangedSPI SPI;

// global state (small)
int ISPError = 0;
int pmode = 0;
unsigned int here;
uint8_t buff[256];
static bool rst_active_high = false;

// Minimal parameter struct
typedef struct param {
  uint8_t devicecode; uint8_t revision; uint8_t progtype; uint8_t parmode;
  uint8_t polling; uint8_t selftimed; uint8_t lockbytes; uint8_t fusebytes;
  uint8_t flashpoll; uint16_t eeprompoll; uint16_t pagesize; uint16_t eepromsize;
  uint32_t flashsize;
} parameter;
parameter param;

// heartbeat / pulsing LED
uint8_t hbval = 128;
int8_t hbdelta = 8;
unsigned long success_until = 0; // transient success green

// LED helpers
// set LED raw
static inline void setLED(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// called regularly to update pulsing/state
void heartbeat() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last < 40) return;
  last = now;
  if (hbval > 220) hbdelta = -abs(hbdelta);
  if (hbval < 20) hbdelta = abs(hbdelta);
  hbval += hbdelta;
}

// Update LED as follows:
// Ready = Blue, Programming = Yellow, Success = Green (transient), Error = Red
void update_led() {
  heartbeat();
  if (ISPError) {
    // LED STATUS: Error
    setLED(hbval, 0, 0); // pulsing red
    return;
  }
  if (millis() < success_until) {
    // LED STATUS: Programming Success
    setLED(0, hbval, 0); // pulsing green (transient)
    return;
  }
  if (pmode) {
    // LED STATUS: Uploading / Programming
    // yellow (red + green) pulsing
    setLED(hbval, hbval / 2, 0);
    return;
  }
  // LED STATUS: Power / Ready
  setLED(0, 0, hbval); // pulsing blue
}

// transient success indicator
void indicate_success(unsigned int ms = 700) {
  success_until = millis() + ms;
}

// Small helpers for serial/spi/protocol
#define beget16(addr) ((*addr) * 256 + *((addr) + 1))
#define SPI_CLOCK (100000UL / 6)

static uint8_t isp_getch() {
  while (!Serial.available());
  return Serial.read();
}

void fill(int n) {
  for (int i = 0; i < n; ++i) buff[i] = isp_getch();
}

uint8_t spi_transaction(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  SPI.transfer(a);
  SPI.transfer(b);
  SPI.transfer(c);
  return SPI.transfer(d);
}

void empty_reply() {
  if (CRC_EOP == isp_getch()) {
    Serial.print((char)STK_INSYNC);
    Serial.print((char)STK_OK);
  } else {
    ISPError++;
    Serial.print((char)STK_NOSYNC);
  }
}

void breply(uint8_t b) {
  if (CRC_EOP == isp_getch()) {
    Serial.print((char)STK_INSYNC);
    Serial.print((char)b);
    Serial.print((char)STK_OK);
  } else {
    ISPError++;
    Serial.print((char)STK_NOSYNC);
  }
}

void set_parameters() {
  param.devicecode = buff[0];
  param.revision = buff[1];
  param.progtype = buff[2];
  param.parmode = buff[3];
  param.polling = buff[4];
  param.selftimed = buff[5];
  param.lockbytes = buff[6];
  param.fusebytes = buff[7];
  param.flashpoll = buff[8];
  param.eeprompoll = beget16(&buff[10]);
  param.pagesize = beget16(&buff[12]);
  param.eepromsize = beget16(&buff[14]);
  param.flashsize = buff[16] * 0x01000000 + buff[17] * 0x00010000 + buff[18] * 0x00000100 + buff[19];
  // rst_active_high = (param.devicecode >= 0xe0);
}

// Reset helper (active HIGH/LOW handled by params)
void reset_target(bool rst) {
  digitalWrite(RESET_PIN, ((rst && rst_active_high) || (!rst && !rst_active_high)) ? HIGH : LOW);
}

// Core ISP operations
void start_pmode() {
  pinMode(RESET_PIN, OUTPUT);   // Set OUTPUT first
  reset_target(true);           // Assert RESET
  SPI.begin();
  SPI.beginTransaction(SPI_CLOCK);
  digitalWrite(SCK_PIN, LOW);
  delay(20);
  reset_target(false);
  delayMicroseconds(100);
  reset_target(true);
  delay(50);
  spi_transaction(0xAC, 0x53, 0x00, 0x00);
  pmode = 1;
}

void end_pmode() {
  pinMode(MOSI_PIN, INPUT);
  pinMode(SCK_PIN, INPUT);
  reset_target(false);
  pinMode(RESET_PIN, INPUT);
  pmode = 0;
}

void universal() {
  fill(4);
  breply(spi_transaction(buff[0], buff[1], buff[2], buff[3]));
}

void flash(uint8_t hilo, unsigned int addr, uint8_t data) {
  spi_transaction(0x40 + 8 * hilo, (addr >> 8) & 0xFF, addr & 0xFF, data);
}

void commit(unsigned int addr) {
  spi_transaction(0x4C, (addr >> 8) & 0xFF, addr & 0xFF, 0);
}

unsigned int current_page() {
  if (param.pagesize == 32) return here & 0xFFFFFFF0;
  if (param.pagesize == 64) return here & 0xFFFFFFE0;
  if (param.pagesize == 128) return here & 0xFFFFFFC0;
  if (param.pagesize == 256) return here & 0xFFFFFF80;
  return here;
}

uint8_t write_flash_pages(int length) {
  int x = 0;
  unsigned int page = current_page();
  while (x < length) {
    if (page != current_page()) {
      commit(page);
      page = current_page();
    }
    flash(0, here, buff[x++]);
    flash(1, here, buff[x++]);
    here++;
  }
  commit(page);
  return STK_OK;
}

void write_flash(int length) {
  fill(length);
  if (CRC_EOP == isp_getch()) {
    Serial.print((char)STK_INSYNC);
    uint8_t res = write_flash_pages(length);
    Serial.print((char)res);
    if (res == STK_OK) indicate_success();
  } else {
    ISPError++;
    Serial.print((char)STK_NOSYNC);
  }
}

#define EECHUNK 32
uint8_t write_eeprom_chunk(unsigned int start, unsigned int length) {
  fill(length);
  for (unsigned int x = 0; x < length; x++) {
    unsigned int addr = start + x;
    spi_transaction(0xC0, (addr >> 8) & 0xFF, addr & 0xFF, buff[x]);
    delay(45);
  }
  return STK_OK;
}

uint8_t write_eeprom(unsigned int length) {
  unsigned int start = here * 2;
  unsigned int remaining = length;
  if (length > param.eepromsize) {
    ISPError++;
    return STK_FAILED;
  }
  while (remaining > EECHUNK) {
    write_eeprom_chunk(start, EECHUNK);
    start += EECHUNK;
    remaining -= EECHUNK;
  }
  write_eeprom_chunk(start, remaining);
  return STK_OK;
}

void program_page() {
  char result = (char)STK_FAILED;
  unsigned int length = 256 * isp_getch();
  length += isp_getch();
  char memtype = isp_getch();
  if (memtype == 'F') {
    write_flash(length);
    return;
  }
  if (memtype == 'E') {
    result = (char)write_eeprom(length);
    if (CRC_EOP == isp_getch()) {
      Serial.print((char)STK_INSYNC);
      Serial.print(result);
      if (result == STK_OK) indicate_success();
    } else {
      ISPError++;
      Serial.print((char)STK_NOSYNC);
    }
    return;
  }
  Serial.print((char)STK_FAILED);
}

uint8_t flash_read(uint8_t hilo, unsigned int addr) {
  return spi_transaction(0x20 + hilo * 8, (addr >> 8) & 0xFF, addr & 0xFF, 0);
}

char flash_read_page(int length) {
  for (int x = 0; x < length; x += 2) {
    uint8_t low = flash_read(0, here);
    Serial.print((char)low);
    uint8_t high = flash_read(1, here);
    Serial.print((char)high);
    here++;
  }
  return STK_OK;
}

char eeprom_read_page(int length) {
  int start = here * 2;
  for (int x = 0; x < length; x++) {
    int addr = start + x;
    uint8_t ee = spi_transaction(0xA0, (addr >> 8) & 0xFF, addr & 0xFF, 0xFF);
    Serial.print((char)ee);
  }
  return STK_OK;
}

void read_page() {
  char result = (char)STK_FAILED;
  int length = 256 * isp_getch();
  length += isp_getch();
  char memtype = isp_getch();
  if (CRC_EOP != isp_getch()) {
    ISPError++;
    Serial.print((char)STK_NOSYNC);
    return;
  }
  Serial.print((char)STK_INSYNC);
  if (memtype == 'F') result = flash_read_page(length);
  if (memtype == 'E') result = eeprom_read_page(length);
  Serial.print(result);
}

void read_signature() {
  if (CRC_EOP != isp_getch()) {
    ISPError++;
    Serial.print((char)STK_NOSYNC);
    return;
  }
  Serial.print((char)STK_INSYNC);
  uint8_t high = spi_transaction(0x30, 0x00, 0x00, 0x00); Serial.print((char)high);
  uint8_t middle = spi_transaction(0x30, 0x00, 0x01, 0x00); Serial.print((char)middle);
  uint8_t low = spi_transaction(0x30, 0x00, 0x02, 0x00); Serial.print((char)low);
  Serial.print((char)STK_OK);
}

// Simple get_version handler required by avrisp() 'A' command
void get_version(uint8_t version_byte) {
  // follow same CRC_EOP / reply convention used elsewhere
  if (CRC_EOP == isp_getch()) {
    Serial.print((char)STK_INSYNC);
    Serial.print((char)version_byte);
    Serial.print((char)STK_OK);
  } else {
    ISPError++;
    Serial.print((char)STK_NOSYNC);
  }
}

void avrisp() {
  uint8_t ch = isp_getch();
  switch (ch) {
    case '0': ISPError = 0; empty_reply(); break;
    case '1': 
    if (isp_getch() == CRC_EOP) { Serial.print((char)STK_INSYNC); Serial.print("AVR ISP"); Serial.print((char)STK_OK); } else { ISPError++; Serial.print((char)STK_NOSYNC); } break;
    case 'A': get_version(isp_getch()); break;
    case 'B': fill(20); set_parameters(); empty_reply(); break;
    case 'E': fill(5); empty_reply(); break;
    case 'P': if (!pmode) start_pmode(); empty_reply(); break;
    case 'U': here = isp_getch(); here += 256 * isp_getch(); empty_reply(); break;
    case 0x60: isp_getch(); isp_getch(); empty_reply(); break;
    case 0x61: isp_getch(); empty_reply(); break;
    case 0x64: program_page(); break;
    case 0x74: read_page(); break;
    case 'V': universal(); break;
    case 'Q': ISPError = 0; end_pmode(); empty_reply(); break;
    case 0x75: read_signature(); break;
    case CRC_EOP: ISPError++; Serial.print((char)STK_NOSYNC); break;
    default:
      ISPError++;
      if (CRC_EOP == isp_getch()) Serial.print((char)STK_UNKNOWN);
      else Serial.print((char)STK_NOSYNC);
  }
}

void setup() {
  Serial.begin(BAUDRATE);
  pixel.begin();
  pixel.clear();
  pixel.show();

  // initial pin config
  pinMode(MOSI_PIN, INPUT);
  pinMode(MISO_PIN, INPUT);
  pinMode(SCK_PIN, INPUT);
  pinMode(RESET_PIN, OUTPUT);
  digitalWrite(RESET_PIN, LOW);

  // brief test pulses (blue, yellow, green) - quick sanity check
  setLED(0, 0, 150); delay(120); setLED(0,0,0); delay(40);
  setLED(150, 120, 0); delay(120); setLED(0,0,0); delay(40);
  setLED(0, 150, 0); delay(120); setLED(0,0,0); delay(40);

  // initialize SPI bitbang
  pinMode(MOSI_PIN, OUTPUT);
  pinMode(SCK_PIN, OUTPUT);
  pinMode(MISO_PIN, INPUT);
  digitalWrite(MOSI_PIN, LOW);
  digitalWrite(SCK_PIN, LOW);
  SPI.begin();
  SPI.beginTransaction(SPI_CLOCK);
}

void loop() {
  update_led();
  if (Serial.available()) avrisp();
}
