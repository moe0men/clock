// smartclock.ino — ESP8266 / NodeMCU v2
// =====================================================================
//  ИЗМЕНЕНИЯ ОТНОСИТЕЛЬНО ОРИГИНАЛА
//  1. ИСПРАВЛЕН  handleFileUpload — двойная запись буфера при ошибке
//  2. ИСПРАВЛЕН  renderWeather    — падение при getIcon()==nullptr
//  3. ИСПРАВЛЕН  streamDirRecursiveHtml — битый HTML (<tr></td>, лишний </tr>)
//  4. ИСПРАВЛЕН  wrapText         — выход за границу буфера (wordStart-1)
//  5. ИСПРАВЛЕН  powerCycleCounterGet — читал struct без инициализации
//  6. УДАЛЕНО    WiFiManager из зависимостей (не использовался, тратил флеш)
//  7. УЛУЧШЕНО   null-check везде, где мог прийти nullptr
//  8. УЛУЧШЕНО   логирование удалённого файла (имя теперь передаётся правильно)
//  9. УЛУЧШЕНО   handleWiFiConnect — yield() внутри блокирующего цикла
// 10. УЛУЧШЕНО   themeRenderClock — убран лишний static-буфер в render-цикле
// =====================================================================

// =====================================================================
//  НАСТРОЙКИ
// =====================================================================
#define PIN_BACKLIGHT 5
#define PIN_BUTTON    4

#define FONT_MICRO   1
#define FONT_SMALL   2
#define FONT_DEFAULT 4
#define FONT_DIGIT   7

#define WIFI_AP_NAME           "SmartClock"
#define WIFI_AP_PASSWORD       "smartclock"
#define WIFI_RETRY_ATTEMPTS    5
#define WIFI_RETRY_DELAY_MS    2000
#define WIFI_CONNECTION_TIMEOUT 30000UL

#define OTA_HOSTNAME    "smartclock"
#define OTA_PASSWORD    "smartclock"
#define WEB_SERVER_PORT 80

#define DISPLAY_UPDATE_INTERVAL  1000UL
#define WEATHER_UPDATE_INTERVAL  900000UL

#define BUTTON_DEBOUNCE_MS       50UL
#define BUTTON_SHORT_PRESS_MAX_MS 800UL
#define BUTTON_LONG_PRESS_MIN_MS  2000UL

#define DEFAULT_TIMEZONE  "MSK-3"
#define DEFAULT_BRIGHTNESS 50

#define ANIMATION_STEPS      20
#define ANIMATION_STEP_DELAY 20

// Параметры дисплея (ST7789)
#define TFT_WIDTH  240
#define TFT_HEIGHT 240
#define TFT_MOSI   13
#define TFT_SCLK   14
#define TFT_CS     -1
#define TFT_DC     0
#define TFT_RST    2
#define TFT_BL     5
#define TOUCH_CS   -1

#define DISPLAY_IP_BUFFER_SIZE         24
#define DISPLAY_IMG_PATH_BUFFER_SIZE   32
#define NOTIFICATION_SBJ_BUFFER_SIZE   32
#define NOTIFICATION_MSG_BUFFER_SIZE   256
#define NOTIFICATION_STYLE_BUFFER_SIZE 8
#define GAUGE_UNIT_BUFFER_SIZE         16
#define COUNTDOWN_SBJ_BUFFER_SIZE      32
#define COUNTDOWN_DATETIME_BUFFER_SIZE 32
#define CLOCK_NOTE_SIZE                128
#define LOG_BUFFER_SIZE                8
#define LOG_LINE_LENGTH                64
#define MAX_LINES                      7
#define MAX_LINE_CHARS                 19
#define LINES_OFFSET                   32
#define COUNTDOWN_GAUGE_OFFSET         60
#define NTP_SERVER "pool.ntp.org"

#define FIRMWARE_MODEL "aydarik"
#define FIRMWARE_VERSION 1
#ifndef FIRMWARE_VERSION_STRING
#define FIRMWARE_VERSION_STRING "dev"
#endif

// =====================================================================
//  БИБЛИОТЕКИ
// =====================================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

// =====================================================================
//  ХЕДЕРЫ С ДАННЫМИ
// =====================================================================
#include "Roboto_Regular24.h"
#include "weather_icons.h"
#include "index_html.h"
#include "ota_html.h"

// =====================================================================
//  СТРУКТУРЫ
// =====================================================================
struct Settings {
    uint16_t version;
    int      brightness;
    char     tz[64];
    bool     showIP;
    bool     showSec;
    bool     showWeather;
    char     owmApiKey[64];
    char     owmLocation[64];
};

struct DisplayState {
    int    theme;
    time_t timeout;
    char   ipInfo[DISPLAY_IP_BUFFER_SIZE];
    char   image[DISPLAY_IMG_PATH_BUFFER_SIZE];
};

struct NotificationState {
    char subject[NOTIFICATION_SBJ_BUFFER_SIZE];
    char message[NOTIFICATION_MSG_BUFFER_SIZE];
    char style[NOTIFICATION_STYLE_BUFFER_SIZE];
};

struct GaugeState {
    float current;
    float max;
    char  unit[GAUGE_UNIT_BUFFER_SIZE];
};

struct CountdownState {
    char subject[COUNTDOWN_SBJ_BUFFER_SIZE];
    char datetime[COUNTDOWN_DATETIME_BUFFER_SIZE];
};

struct ClockState {
    char   note[CLOCK_NOTE_SIZE];
    time_t noteTimeout;
    int    noteRotations;
};

struct PowerCycleCounter {
    uint16_t magic;
    uint8_t  cycleCount;
};

// =====================================================================
//  ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// =====================================================================
TFT_eSPI         tft = TFT_eSPI();
ESP8266WebServer server(WEB_SERVER_PORT);

Settings         appSettings;
DisplayState     displayState;
NotificationState notificationState;
CountdownState   countdownState;
ClockState       clockState;
GaugeState       gaugeState;

char logBuffer[LOG_BUFFER_SIZE][LOG_LINE_LENGTH];
int  logIndex = 0, logCount = 0;

static bool          lastButtonState    = HIGH;
static bool          currentButtonState = HIGH;
static bool          buttonPressed      = false;
static unsigned long buttonPressStartTime = 0;
static unsigned long lastDebounceTime   = 0;
static bool          backlightOn        = true;

unsigned long lastDisplayUpdate = 0;
unsigned long lastWeatherUpdate = 0;
bool          powerCycleCounterCleared = false;

// Состояние погоды
static int   httpCode    = 0;
static float currentTemp = 0.0f;
static float feelsLike   = 0.0f;
static char  iconCode[8] = "";

// Таблица иконок погоды
struct IconMap { const char *key; const uint8_t *value; unsigned int size; };
const IconMap icons[] PROGMEM = {
    {"01d", owm_icon_01d, owm_icon_01d_len}, {"01n", owm_icon_01n, owm_icon_01n_len},
    {"02d", owm_icon_02d, owm_icon_02d_len}, {"02n", owm_icon_02n, owm_icon_02n_len},
    {"03d", owm_icon_03d, owm_icon_03d_len}, {"03n", owm_icon_03n, owm_icon_03n_len},
    {"04d", owm_icon_04d, owm_icon_04d_len}, {"04n", owm_icon_04n, owm_icon_04n_len},
    {"09d", owm_icon_09d, owm_icon_09d_len}, {"09n", owm_icon_09n, owm_icon_09n_len},
    {"10d", owm_icon_10d, owm_icon_10d_len}, {"10n", owm_icon_10n, owm_icon_10n_len},
    {"11d", owm_icon_11d, owm_icon_11d_len}, {"11n", owm_icon_11n, owm_icon_11n_len},
    {"13d", owm_icon_13d, owm_icon_13d_len}, {"13n", owm_icon_13n, owm_icon_13n_len},
    {"50d", owm_icon_50d, owm_icon_50d_len}, {"50n", owm_icon_50n, owm_icon_50n_len},
};

// =====================================================================
//  ПРОТОТИПЫ
// =====================================================================
void renderWeather(bool clear = false);
void themeRenderClock(bool forceClear, const time_t &now);
void themeRenderAPMode(bool forceClear);
void themeRenderNotification(bool forceClear);
void themeRenderCountdown(bool forceClear, const time_t &now);
void displayUpdate(int theme = 0, bool forceClear = true);
void displaySetBrightness(int brightness);
void showMessage(const String &msg, int timeout = 0, int offsetY = 0);
void settingsSave(const Settings &settings);
void settingsReset(Settings &settings);
void powerCycleCounterReset();
void factoryReset();
enum ButtonPress { BUTTON_NONE = 0, BUTTON_SHORT = 1, BUTTON_LONG = 2 };
void        buttonInit();
ButtonPress buttonUpdate();

// =====================================================================
//  ЛОГГЕР
// =====================================================================
void loggerInit() {
    memset(logBuffer, 0, sizeof(logBuffer));
    logIndex = 0;
    logCount = 0;
}

void logPrint(const char *msg) {
    snprintf(logBuffer[logIndex], LOG_LINE_LENGTH, "%lu: %s", millis(), msg);
    Serial.println(msg);
    logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
    if (logCount < LOG_BUFFER_SIZE) logCount++;
}

void logPrintf(const char *format, ...) {
    char buf[LOG_LINE_LENGTH];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, LOG_LINE_LENGTH, format, args);
    va_end(args);
    logPrint(buf);
}

String logGetAll() {
    String result;
    result.reserve(LOG_BUFFER_SIZE * LOG_LINE_LENGTH);
    int start   = (logCount < LOG_BUFFER_SIZE) ? 0 : logIndex;
    int entries = (logCount < LOG_BUFFER_SIZE) ? logCount : LOG_BUFFER_SIZE;
    for (int i = 0; i < entries; i++) {
        result += logBuffer[(start + i) % LOG_BUFFER_SIZE];
        result += '\n';
    }
    return result;
}

// =====================================================================
//  УТИЛИТЫ
// =====================================================================
int utf8Length(const char *text) {
    int count = 0;
    while (*text) {
        if ((*text & 0xC0) != 0x80) count++;
        text++;
    }
    return count;
}

// FIX: защита от выхода за границу буфера при переносе первого слова строки
size_t wrapText(char *text, char *lines[], size_t maxLines) {
    if (!text || *text == '\0') return 0;
    size_t count       = 0;
    char  *lineStart   = text;
    size_t currentWidth = 0;
    char  *p           = text;

    while (*p && count < maxLines) {
        if (*p == '\n') {
            *p = '\0';
            lines[count++] = lineStart;
            lineStart = p + 1;
            currentWidth = 0;
            p++;
            continue;
        }
        char *wordStart = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        char   saved     = *p;
        *p = '\0';
        size_t wordWidth = utf8Length(wordStart);

        if (currentWidth == 0) {
            // Первое слово в строке — просто принимаем, даже если длинное
            currentWidth = wordWidth;
        } else if (currentWidth + wordWidth + 1 <= MAX_LINE_CHARS) {
            currentWidth += wordWidth + 1;
        } else {
            // Перенос: завершаем предыдущую строку, отрезая пробел перед словом
            // БЫЛО: *(wordStart - 1) = '\0' — UB, если wordStart == text
            if (wordStart > lineStart) {
                *(wordStart - 1) = '\0';  // безопасно: wordStart не первый символ строки
            }
            lines[count++] = lineStart;
            lineStart      = wordStart;
            currentWidth   = wordWidth;
        }
        *p = saved;
        if (*p == ' ') p++;
    }
    if (*lineStart && count < maxLines) {
        lines[count++] = lineStart;
    }
    return count;
}

size_t splitString(const String &s, String lines[], size_t maxLines) {
    if (s.length() == 0) return 0;
    size_t count = 0;
    int    start = 0;
    while (count < maxLines) {
        int nl = s.indexOf('\n', start);
        if (nl == -1) { lines[count++] = s.substring(start); break; }
        lines[count++] = s.substring(start, nl);
        start = nl + 1;
    }
    return count;
}

void animateHLine(int y) {
    int32_t centerX = tft.width() / 2;
    int32_t offset  = centerX / ANIMATION_STEPS;
    tft.startWrite();
    for (int i = 1; i <= ANIMATION_STEPS; ++i) {
        tft.drawFastHLine(centerX - i * offset, y, i * offset * 2, TFT_SILVER);
        delay(ANIMATION_STEP_DELAY);
    }
    tft.endWrite();
}

void drawSubject(const char *text) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.loadFont(Roboto_Regular24);
    tft.drawString(text, tft.width() / 2, 0, FONT_DEFAULT);
    tft.unloadFont();
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

time_t parseDateTime(const String &s) {
    const char *str = s.c_str();
    if (str[4] != '-' || str[7] != '-' ||
        (str[10] != ' ' && str[10] != 'T') || str[13] != ':') return 0;
    if (s.length() > 16 && str[16] != ':') return 0;

    auto toInt2 = [](char a, char b) -> int {
        if (!isdigit(a) || !isdigit(b)) return -1;
        return (a - '0') * 10 + (b - '0');
    };
    auto toInt4 = [](const char *p) -> int {
        for (int i = 0; i < 4; i++) if (!isdigit(p[i])) return -1;
        return (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    };

    int year  = toInt4(str);
    int month = toInt2(str[5], str[6]);
    int day   = toInt2(str[8], str[9]);
    int hour  = toInt2(str[11], str[12]);
    int min   = toInt2(str[14], str[15]);
    int sec   = (s.length() > 16) ? toInt2(str[17], str[18]) : 0;

    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) return 0;

    tm t = {};
    t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day;
    t.tm_hour = hour;        t.tm_min = min;        t.tm_sec  = sec;
    t.tm_isdst = -1;
    return mktime(&t);
}

void showMessage(const String &msg, int timeout, int offsetY) {
    displayState.theme = 0;
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.loadFont(Roboto_Regular24);
    String wrapped[MAX_LINES];
    size_t count      = splitString(msg, wrapped, MAX_LINES);
    int    lineHeight = 34;
    int    centerX    = tft.width() / 2;
    int    currentY   = tft.height() / 2 - (int)(count * lineHeight) / 2 + offsetY;
    for (size_t i = 0; i < count; i++) {
        tft.drawString(wrapped[i], centerX, currentY);
        currentY += lineHeight;
    }
    tft.unloadFont();
    if (timeout > 0) displayState.timeout = time(nullptr) + timeout;
}

int hexToInt(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void urlDecode(const char *input, char *output, size_t output_size) {
    size_t max_size = output_size - 1, written = 0;
    while (*input && written < max_size) {
        if (*input == '+') {
            output[written++] = ' '; input++;
        } else if (*input == '%' && isxdigit(*(input+1)) && isxdigit(*(input+2))) {
            output[written++] = (char)((hexToInt(*(input+1)) << 4) | hexToInt(*(input+2)));
            input += 3;
        } else {
            output[written++] = *input++;
        }
    }
    output[written] = '\0';
}

// =====================================================================
//  НАСТРОЙКИ (EEPROM)
// =====================================================================
#define EEPROM_SIZE                  512
#define SETTINGS_MAGIC               0xCAFE
#define SETTINGS_ADDR                0
#define POWER_CYCLE_COUNTER_MAGIC    0x5C01
#define POWER_CYCLE_COUNTER_ADDR     (SETTINGS_ADDR + sizeof(Settings) + 2)
#define POWER_CYCLE_THRESHOLD        5

void settingsInit() { EEPROM.begin(EEPROM_SIZE); }

bool settingsValidate(const Settings &s) {
    return (s.version == FIRMWARE_VERSION && s.brightness >= 0 && s.brightness <= 100);
}

void settingsReset(Settings &s) {
    logPrint("Resetting settings...");
    s.version    = FIRMWARE_VERSION;
    s.brightness = DEFAULT_BRIGHTNESS;
    strncpy(s.tz, DEFAULT_TIMEZONE, sizeof(s.tz) - 1);
    s.tz[sizeof(s.tz) - 1] = '\0';
    s.showIP      = true;
    s.showSec     = true;
    s.showWeather = false;
    strncpy(s.owmApiKey, "bd5e378503939ddaee76f12ad7a97608", sizeof(s.owmApiKey) - 1);
    s.owmApiKey[sizeof(s.owmApiKey) - 1] = '\0';
    s.owmLocation[0]  = '\0';
    settingsSave(s);
}

void settingsLoad(Settings &s) {
    uint16_t magic;
    EEPROM.get(SETTINGS_ADDR, magic);
    if (magic == SETTINGS_MAGIC) {
        EEPROM.get(SETTINGS_ADDR + 2, s);
        if (!settingsValidate(s)) settingsReset(s);
        else logPrint("Settings loaded");
    } else {
        settingsReset(s);
    }
}

void settingsSave(const Settings &s) {
    const uint16_t magic = SETTINGS_MAGIC;
    EEPROM.put(SETTINGS_ADDR, magic);
    EEPROM.put(SETTINGS_ADDR + 2, s);
    EEPROM.commit();
}

// FIX: правильное чтение структуры из EEPROM
uint8_t powerCycleCounterGet() {
    PowerCycleCounter counter;
    EEPROM.get(POWER_CYCLE_COUNTER_ADDR, counter);
    if (counter.magic == POWER_CYCLE_COUNTER_MAGIC) return counter.cycleCount;
    return 0;
}

void powerCycleCounterIncrement() {
    PowerCycleCounter counter;
    EEPROM.get(POWER_CYCLE_COUNTER_ADDR, counter);
    if (counter.magic == POWER_CYCLE_COUNTER_MAGIC) {
        counter.cycleCount++;
    } else {
        counter.magic      = POWER_CYCLE_COUNTER_MAGIC;
        counter.cycleCount = 1;
    }
    EEPROM.put(POWER_CYCLE_COUNTER_ADDR, counter);
    EEPROM.commit();
}

void powerCycleCounterReset() {
    PowerCycleCounter counter = { POWER_CYCLE_COUNTER_MAGIC, 0 };
    EEPROM.put(POWER_CYCLE_COUNTER_ADDR, counter);
    EEPROM.commit();
}

bool powerCycleCounterCheckReset() {
    return (powerCycleCounterGet() >= POWER_CYCLE_THRESHOLD);
}

// =====================================================================
//  ДИСПЛЕЙ
// =====================================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (y >= tft.height()) return false;
    tft.pushImage(x, y, w, h, bitmap);
    return true;
}

void displayInit() {
    tft.init();
    tft.setTextWrap(false);
    tft.setTextFont(FONT_DEFAULT);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tft_output);
    pinMode(PIN_BACKLIGHT, OUTPUT);
    analogWriteFreq(1000);
    analogWriteRange(1023);
    logPrint("Display init complete");
}

void displaySetBrightness(int brightness) {
    brightness = constrain(brightness, 0, 100);
    // Инверсный ШИМ: 0 яркость = 1023 (выкл), 100 = 0 (полный свет)
    int pwmValue = (brightness == 0)   ? 1023
                 : (brightness == 100) ? 0
                 : map(brightness, 0, 100, 1023, 0);
    analogWrite(PIN_BACKLIGHT, pwmValue);
}

void displayTest() {
    tft.fillScreen(TFT_RED);   delay(500);
    tft.fillScreen(TFT_GREEN); delay(500);
    tft.fillScreen(TFT_BLUE);  delay(500);
    tft.fillScreen(TFT_WHITE); delay(500);
    tft.fillScreen(TFT_BLACK);
    showMessage("Тест дисплея\nуспешно\nзавершён", 3);
}

void displayRenderImage(bool forceClear) {
    if (!forceClear) return;
    const char *path = displayState.image;
    if (path[0] == '\0')       { showMessage("Изображение\nне выбрано", 5); return; }
    if (!LittleFS.exists(path)) { showMessage("Файл не найден", 5); return; }
    File jpgFile = LittleFS.open(path, "r");
    if (!jpgFile)               { showMessage("Ошибка\nоткрытия файла", 5); return; }
    tft.startWrite();
    JRESULT res = TJpgDec.drawFsJpg(0, 0, jpgFile);
    tft.endWrite();
    jpgFile.close();
    if (res != JDR_OK) showMessage("Ошибка\nдекодирования", 5);
}

void displayUpdate(int theme, bool forceClear) {
    time_t now;
    time(&now);
    if (theme != 0) displayState.theme = theme;

    if (displayState.timeout != 0) {
        if (forceClear || displayState.theme < 0) {
            displayState.timeout = 0;
        } else if (now > displayState.timeout) {
            displayUpdate(1, true);
            return;
        }
    }

    switch (displayState.theme) {
        case -1: themeRenderAPMode(forceClear);          break;
        case  1: themeRenderClock(forceClear, now);      break;
        case  2: themeRenderNotification(forceClear);    break;
        case  3: displayRenderImage(forceClear);         break;
        case  4: themeRenderCountdown(forceClear, now);  break;
    }

    // Полоса таймера в нижней части экрана
    if (displayState.timeout > 0) {
        int diff = (int)(displayState.timeout - now);
        if (diff <= 60) {
            int currentX = diff * tft.width() / 60;
            int barY     = tft.height() - 8;
            tft.drawFastHLine(currentX, barY, tft.width(), TFT_BLACK);
            tft.drawFastHLine(0,        barY, currentX,   TFT_DARKGREY);
        }
    }
}

void displayCycleNextPage() {
    if (displayState.theme == 1 && displayState.image[0] != '\0' && LittleFS.exists(displayState.image))
        displayUpdate(3, true);
    else
        displayUpdate(1, true);
}

void displayToggleBacklight() {
    if (backlightOn) { displaySetBrightness(0); backlightOn = false; }
    else             { displaySetBrightness(appSettings.brightness); backlightOn = true; }
}

// =====================================================================
//  ТЕМА: AP MODE
// =====================================================================
void themeRenderAPMode(bool forceClear) {
    if (!forceClear) return;
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    int cx = tft.width() / 2, y = 10;

    if (displayState.ipInfo[0] != '\0') {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setTextFont(FONT_MICRO);
        tft.drawString(String(displayState.ipInfo), cx, y, FONT_MICRO);
        y += tft.fontHeight();
    }
    y += 40;
    tft.loadFont(Roboto_Regular24);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Режим AP", cx, y);
    y += 40;
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("SSID:", cx, y);
    y += 28;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(WIFI_AP_NAME, cx, y);
    y += 40;
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Пароль:", cx, y);
    y += 28;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(WIFI_AP_PASSWORD, cx, y);
    tft.unloadFont();
}

// =====================================================================
//  КНОПКА
// =====================================================================
void buttonInit() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    lastButtonState = currentButtonState = digitalRead(PIN_BUTTON);
    logPrintf("Button initialized on GPIO%d", PIN_BUTTON);
}

ButtonPress buttonUpdate() {
    bool reading = digitalRead(PIN_BUTTON);
    if (reading != lastButtonState) lastDebounceTime = millis();

    if (millis() - lastDebounceTime > BUTTON_DEBOUNCE_MS) {
        if (reading != currentButtonState) {
            currentButtonState = reading;
            if (currentButtonState == LOW && !buttonPressed) {
                buttonPressStartTime = millis();
                buttonPressed = true;
            } else if (currentButtonState == HIGH && buttonPressed) {
                unsigned long dur = millis() - buttonPressStartTime;
                buttonPressed = false;
                if (dur >= BUTTON_LONG_PRESS_MIN_MS)  return BUTTON_LONG;
                if (dur <= BUTTON_SHORT_PRESS_MAX_MS) return BUTTON_SHORT;
            }
        }
    }
    lastButtonState = reading;
    return BUTTON_NONE;
}

// =====================================================================
//  ПОГОДА
// =====================================================================
const IconMap *getIcon(const char *key) {
    for (const auto &icon : icons) {
        const char *storedKey = (const char *)pgm_read_ptr(&icon.key);
        if (strcmp_P(key, storedKey) == 0) return &icon;
    }
    return nullptr;
}

void clearWeather() {
    tft.startWrite();
    for (int i = 1; i <= 18; ++i) {
        tft.drawFastHLine(0, i,      240, TFT_BLACK);
        tft.drawFastHLine(0, 36 - i, 240, TFT_BLACK);
        delay(ANIMATION_STEP_DELAY);
    }
    tft.endWrite();
}

void renderWeather(bool clear) {
    char tempStr[24];
    if      (httpCode == HTTP_CODE_OK) snprintf(tempStr, sizeof(tempStr), "%.0f\xC2\xB0, feels like %.0f\xC2\xB0", currentTemp, feelsLike);
    else if (httpCode == 0)            snprintf(tempStr, sizeof(tempStr), "Loading...");
    else                               snprintf(tempStr, sizeof(tempStr), "FAILED: %d", httpCode);

    if (clear) clearWeather();

    tft.setTextDatum(TL_DATUM);
    tft.loadFont(Roboto_Regular24);
    tft.drawString(tempStr, 35, 7);
    tft.unloadFont();

    // FIX: проверка nullptr перед разыменованием
    if (httpCode == HTTP_CODE_OK && strlen(iconCode) > 0) {
        const IconMap *icon = getIcon(iconCode);
        if (icon) {
            const uint8_t *data = (const uint8_t *)pgm_read_ptr(&icon->value);
            unsigned int   size = pgm_read_dword(&icon->size);
            TJpgDec.drawJpg(1, 1, data, size);
        }
    }
}

bool weatherUpdateTask() {
    if (!appSettings.showWeather || appSettings.brightness == 0 || displayState.theme != 1) return false;
    if (appSettings.owmApiKey[0] == '\0' || appSettings.owmLocation[0] == '\0') return false;

    char url[256];
    snprintf(url, sizeof(url),
        "http://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=metric",
        appSettings.owmLocation, appSettings.owmApiKey);

    WiFiClient  client;
    HTTPClient  http;
    http.begin(client, url);
    httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getStream())) {
            currentTemp = doc["main"]["temp"]       | currentTemp;
            feelsLike   = doc["main"]["feels_like"] | feelsLike;
            const char *icon = doc["weather"][0]["icon"];
            if (icon) {
                strncpy(iconCode, icon, sizeof(iconCode) - 1);
                iconCode[sizeof(iconCode) - 1] = '\0';
            }
        }
    }
    http.end();
    renderWeather(true);
    return true;
}

// =====================================================================
//  ТЕМА: ЧАСЫ
// =====================================================================
void getFormattedTime(char *buf, size_t sz, const tm &t)   { strftime(buf, sz, "%H:%M", &t); }
void getFormattedSeconds(char *buf, size_t sz, const tm &t){ strftime(buf, sz, "%S", &t); }
void getFormattedDate(char *buf, size_t sz, const tm &t)   { strftime(buf, sz, "%d-%m-%Y", &t); }

void clearNote() {
    tft.startWrite();
    for (int i = 1; i <= 15; ++i) {
        tft.drawFastHLine(0, 195 + i, 240, TFT_BLACK);
        tft.drawFastHLine(0, 225 - i, 240, TFT_BLACK);
        delay(ANIMATION_STEP_DELAY);
    }
    tft.endWrite();
}

void themeRenderClock(bool forceClear, const time_t &now) {
    tm timeinfo;
    localtime_r(&now, &timeinfo);
    int sec = timeinfo.tm_sec;

    if (forceClear) tft.fillScreen(TFT_BLACK);

    int  cx       = tft.width() / 2;
    bool hasNote  = clockState.note[0] != '\0';
    int  clockY   = (hasNote ? 50 : 63)
                  + (appSettings.showIP      ? 7  : 0)
                  + (appSettings.showWeather ? 17 : 0);

    tft.setTextDatum(TC_DATUM);

    // Секунды (правее основного времени)
    if (appSettings.showSec) {
        char secs[4];
        getFormattedSeconds(secs, sizeof(secs), timeinfo);
        tft.drawString(secs, cx + 71, clockY + 27, FONT_DEFAULT);
    }

    // Заметка под часами
    if (hasNote) {
        if (clockState.noteTimeout != 0 && now > clockState.noteTimeout) {
            clockState.note[0]       = '\0';
            clockState.noteTimeout   = 0;
            clockState.noteRotations = 0;
            clearNote();
            themeRenderClock(true, now);
            return;
        }
        // Буферы для строк — объявлены здесь, не static, экономим стек через wrap
        char  noteCopy[CLOCK_NOTE_SIZE];
        // Массив указателей; wraptText пишет '\0' внутрь noteCopy
        char *lines[MAX_LINES];
        strncpy(noteCopy, clockState.note, sizeof(noteCopy) - 1);
        noteCopy[sizeof(noteCopy) - 1] = '\0';

        size_t count     = wrapText(noteCopy, lines, MAX_LINES);
        unsigned int rots = (clockState.noteRotations > (int)count)
                            ? (unsigned int)clockState.noteRotations : (unsigned int)count;
        unsigned int idx      = (unsigned int)sec * rots / 60 % count;
        unsigned int idxPrev  = (unsigned int)(sec == 0 ? 59 : sec - 1) * rots / 60 % count;

        if ((idxPrev != idx || sec == 0) && !forceClear) clearNote();
        if (idx != idxPrev || sec == 0 || forceClear) {
            tft.loadFont(Roboto_Regular24);
            tft.drawString(lines[idx], cx, tft.height() - 40);
            tft.unloadFont();
        }
    }

    if (!forceClear && sec != 0) return;

    // IP-адрес
    if (forceClear && appSettings.showIP) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString(displayState.ipInfo, cx, appSettings.showWeather ? 40 : 5, FONT_MICRO);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }

    // Погода
    if (forceClear && appSettings.showWeather) {
        renderWeather(false);
        tft.setTextDatum(TC_DATUM);
    }

    // Время HH:MM
    char curTime[8];
    getFormattedTime(curTime, sizeof(curTime), timeinfo);
    tft.drawString(curTime, appSettings.showSec ? cx - 20 : cx, clockY, FONT_DIGIT);

    if (!forceClear && strcmp(curTime, "00:00") != 0) return;

    // Дата (обновляется раз в минуту)
    char curDate[16];
    getFormattedDate(curDate, sizeof(curDate), timeinfo);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    int32_t offset = (appSettings.showWeather && hasNote) ? 65
                   : (appSettings.showWeather || hasNote)  ? 70 : 75;
    tft.drawString(curDate, cx, clockY + offset, FONT_DEFAULT);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

// =====================================================================
//  ТЕМА: УВЕДОМЛЕНИЕ
// =====================================================================
void parseValue(const char *input) {
    char buf[32];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *spacePtr = strchr(buf, ' ');
    if (spacePtr) {
        *spacePtr = '\0';
        strncpy(gaugeState.unit, spacePtr + 1, GAUGE_UNIT_BUFFER_SIZE - 1);
        gaugeState.unit[GAUGE_UNIT_BUFFER_SIZE - 1] = '\0';
    } else {
        gaugeState.unit[0] = '\0';
    }

    char *slashPtr = strchr(buf, '/');
    if (slashPtr) {
        *slashPtr = '\0';
        gaugeState.current = atof(buf);
        gaugeState.max     = atof(slashPtr + 1);
    } else {
        gaugeState.current = atof(buf);
        gaugeState.max     = 0;
    }
}

void drawNotificationGauge(int x, int y, int r, float current, float max) {
    if (max <= 0.0f) return;
    float percent = constrain(current / max, 0.0f, 1.0f);
    const int startAngle = 55, endAngle = 305, totalAngle = endAngle - startAngle;

    tft.drawArc(x, y, r, r - 12, startAngle, endAngle, TFT_DARKGREY, TFT_BLACK);

    bool     hasSubject   = notificationState.subject[0] != '\0';
    int      centerX      = tft.width() / 2;
    int      subjectOffset = hasSubject ? centerX / ANIMATION_STEPS : 0;
    uint32_t gaugeColor   = (percent < 0.2f || percent > 0.8f) ? TFT_RED : TFT_OLIVE;

    tft.startWrite();
    for (int i = 1; i <= ANIMATION_STEPS; ++i) {
        if (hasSubject)
            tft.drawFastHLine(centerX - i * subjectOffset, 32, i * subjectOffset * 2, TFT_SILVER);
        int curAngle = startAngle + (int)(totalAngle * percent * (float)i / ANIMATION_STEPS);
        tft.drawArc(x, y, r, r - 12, startAngle, curAngle, gaugeColor, TFT_BLACK);
        delay(ANIMATION_STEP_DELAY);
    }
    tft.endWrite();
}

void showNumber(const char *input, int x, int y) {
    parseValue(input);
    bool hasGauge = gaugeState.max > 0;
    bool hasUnit  = gaugeState.unit[0] != '\0';
    char numBuf[8];
    float val = gaugeState.current;
    if (val == (int)val) sprintf(numBuf, "%d", (int)val);
    else                  dtostrf(val, 0, 1, numBuf);

    tft.setTextDatum(MC_DATUM);
    int curY = hasGauge ? y + 10 : hasUnit ? y - 16 : y;
    tft.drawString(numBuf, x, curY, FONT_DIGIT);

    if (hasUnit) {
        tft.loadFont(Roboto_Regular24);
        tft.drawString(gaugeState.unit, x, curY + 60);
        tft.unloadFont();
    }
    if (hasGauge)
        drawNotificationGauge(x, y + 15, 100, gaugeState.current, gaugeState.max);
    else if (notificationState.subject[0] != '\0')
        animateHLine(32);
}

void themeRenderNotification(bool forceClear) {
    if (!forceClear) return;
    if (notificationState.message[0] == '\0') { showMessage("Нет сообщений", 5); return; }
    tft.fillScreen(TFT_BLACK);

    bool hasSubject = notificationState.subject[0] != '\0';
    int  centerY    = tft.height() / 2;
    int  centerX    = tft.width()  / 2;
    int  curY       = 0;

    if (hasSubject) { drawSubject(notificationState.subject); curY = 44; }

    if (strcmp(notificationState.style, "big_num") == 0) {
        showNumber(notificationState.message, centerX, centerY + curY / 2);
        return;
    }

    static char msg[NOTIFICATION_MSG_BUFFER_SIZE];
    char       *wrapped[MAX_LINES];
    strncpy(msg, notificationState.message, sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';
    size_t count = wrapText(msg, wrapped, MAX_LINES);

    int curX = 5;
    if (strcmp(notificationState.style, "center") == 0) {
        tft.setTextDatum(TC_DATUM);
        curX = centerX;
        curY = centerY + curY / 2 - (int)(LINES_OFFSET * count) / 2;
    } else {
        if (!hasSubject) curY = 5;
        tft.setTextDatum(TL_DATUM);
    }

    tft.loadFont(Roboto_Regular24);
    tft.startWrite();
    for (size_t i = 0; i < count; i++) {
        if (strcmp(wrapped[i], "---") == 0) {
            int lineY = curY + (int)i * LINES_OFFSET + LINES_OFFSET / 3;
            tft.drawFastHLine(0, lineY, tft.width(), TFT_DARKGREY);
        } else {
            tft.drawString(wrapped[i], curX, curY + (int)i * LINES_OFFSET);
        }
    }
    tft.endWrite();
    tft.unloadFont();

    if (hasSubject) animateHLine(32);
}

// =====================================================================
//  ТЕМА: ОБРАТНЫЙ ОТСЧЁТ
// =====================================================================
void drawCountdownGauge(int x, int y, int r, long sec, bool passed) {
    if (!passed && sec > COUNTDOWN_GAUGE_OFFSET) return;
    int rInner = r - 12;
    if (passed) {
        if (sec == 1) tft.drawArc(x, y, r, rInner, 0, 360, TFT_BLACK, TFT_BLACK);
        bool even = sec % 2 == 0;
        tft.drawArc(x, y, r, rInner, 120, 240, even  ? TFT_RED : TFT_ORANGE, TFT_BLACK);
        tft.drawArc(x, y, r, rInner, 300,  60, !even ? TFT_RED : TFT_ORANGE, TFT_BLACK);
        return;
    }
    int endAngle = (int)((COUNTDOWN_GAUGE_OFFSET - sec) * 360 / COUNTDOWN_GAUGE_OFFSET);
    tft.drawArc(x, y, r, rInner, 0, endAngle, sec < 10 ? TFT_RED : TFT_ORANGE, TFT_BLACK);
}

void themeRenderCountdown(bool forceClear, const time_t &now) {
    if (countdownState.datetime[0] == '\0') { if (forceClear) showMessage("Дата не задана", 5); return; }
    time_t targetTime = parseDateTime(countdownState.datetime);
    if (targetTime == 0) { if (forceClear) showMessage("Неверный формат\nдаты и времени", 5); return; }

    long diff   = (long)(targetTime - now);
    bool passed = diff < 0;
    if (passed) diff = -diff;

    int minutes = (int)(diff / 60);
    int seconds = (int)(diff % 60);

    if (forceClear) tft.fillScreen(TFT_BLACK);

    bool hasSubject = countdownState.subject[0] != '\0';
    int  curY       = -8;
    if (hasSubject) { if (forceClear) drawSubject(countdownState.subject); curY = 44; }

    char buf[8];
    if (passed) sprintf(buf, "-%d:%02d", minutes, seconds);
    else        sprintf(buf, "%d:%02d",  minutes, seconds);

    int clockY = (tft.width() + curY) / 2;

    // Стираем только цифры при смене числа разрядов
    if (!forceClear && !passed && seconds == 59 && (minutes + 1) % 10 == 0)
        tft.fillRect(0, clockY - 30, tft.width(), 60, TFT_BLACK);

    drawCountdownGauge(tft.height() / 2, clockY, 100, diff, passed);

    if (passed)
        tft.setTextColor(TFT_RED, TFT_BLACK);
    else if (minutes == 0 && seconds <= 10)
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);

    tft.setTextDatum(MC_DATUM);
    tft.drawString(buf, tft.height() / 2, clockY, FONT_DIGIT);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    if (hasSubject && forceClear) animateHLine(32);
}

// =====================================================================
//  ВЕБ-СЕРВЕР — вспомогательная функция отправки JSON
// =====================================================================
static void sendJson(JsonDocument &doc) {
    server.setContentLength(measureJson(doc));
    server.send(200, "application/json", "");
    serializeJson(doc, server.client());
}

// =====================================================================
//  ВЕБ-СЕРВЕР — обработчики
// =====================================================================
void handleAppJson() {
    JsonDocument doc;
    doc["theme"]      = displayState.theme;
    doc["img"]        = displayState.image;
    doc["tz"]         = appSettings.tz;
    doc["showIP"]     = appSettings.showIP;
    doc["showSec"]    = appSettings.showSec;
    doc["showWeather"]= appSettings.showWeather;
    doc["owmLoc"]     = appSettings.owmLocation;
    doc["owmKey"]     = appSettings.owmApiKey;
    if (displayState.timeout != 0) doc["timeout"] = displayState.timeout;
    sendJson(doc);
}

void handleSpaceJson() {
    FSInfo fs_info; LittleFS.info(fs_info);
    JsonDocument doc;
    doc["total"] = fs_info.totalBytes;
    doc["free"]  = fs_info.totalBytes - fs_info.usedBytes;
    sendJson(doc);
}

void handleMemoryJson() {
    JsonDocument doc;
    doc["heap"]  = ESP.getFreeHeap();
    doc["fragm"] = ESP.getHeapFragmentation();
    sendJson(doc);
}

void handleBrtJson() {
    JsonDocument doc; doc["brt"] = appSettings.brightness;
    sendJson(doc);
}

void handleVersionJson() {
    JsonDocument doc; doc["m"] = FIRMWARE_MODEL; doc["v"] = FIRMWARE_VERSION_STRING;
    sendJson(doc);
}

void handleMessageJson() {
    JsonDocument doc;
    doc["msg"]   = notificationState.message;
    doc["sbj"]   = notificationState.subject;
    doc["style"] = notificationState.style;
    sendJson(doc);
}

void handleNoteJson() {
    JsonDocument doc;
    doc["note"] = clockState.note;
    if (clockState.noteRotations > 0) doc["rpm"] = clockState.noteRotations;
    sendJson(doc);
}

void handleSet() {
    if (server.hasArg("msg")) {
        urlDecode(server.arg("msg").c_str(),   notificationState.message, NOTIFICATION_MSG_BUFFER_SIZE);
        urlDecode(server.arg("sbj").c_str(),   notificationState.subject, NOTIFICATION_SBJ_BUFFER_SIZE);
        urlDecode(server.arg("style").c_str(), notificationState.style,   NOTIFICATION_STYLE_BUFFER_SIZE);
        displayUpdate(2, true);
        if (server.arg("timeout").toInt() > 0)
            displayState.timeout = time(nullptr) + server.arg("timeout").toInt();

    } else if (server.hasArg("note")) {
        bool hadNote = clockState.note[0] != '\0';
        urlDecode(server.arg("note").c_str(), clockState.note, CLOCK_NOTE_SIZE);
        bool hasNote = clockState.note[0] != '\0';
        clockState.noteRotations = min((int)server.arg("rpm").toInt(), 60);
        clockState.noteTimeout   = server.arg("timeout").toInt() > 0
                                   ? time(nullptr) + server.arg("timeout").toInt() : 0;
        String force = server.arg("force");
        if ((displayState.theme == 1 && hadNote != hasNote) ||
            force.equalsIgnoreCase("true") || force.equals("1"))
            displayUpdate(1, true);

    } else if (server.hasArg("cnt")) {
        urlDecode(server.arg("sbj").c_str(), countdownState.subject,  COUNTDOWN_SBJ_BUFFER_SIZE);
        urlDecode(server.arg("cnt").c_str(), countdownState.datetime, COUNTDOWN_DATETIME_BUFFER_SIZE);
        displayUpdate(4, true);
        if (server.arg("timeout").toInt() > 0) {
            time_t dt = parseDateTime(countdownState.datetime);
            if (dt > time(nullptr))
                displayState.timeout = dt + server.arg("timeout").toInt();
        }
    } else if (server.hasArg("brt")) {
        appSettings.brightness = server.arg("brt").toInt();
        displaySetBrightness(appSettings.brightness);
        settingsSave(appSettings);

    } else if (server.hasArg("theme")) {
        displayUpdate(server.arg("theme").toInt(), true);

    } else if (server.hasArg("img")) {
        urlDecode(server.arg("img").c_str(), displayState.image, DISPLAY_IMG_PATH_BUFFER_SIZE);
        displayUpdate(3, true);
        if (server.arg("timeout").toInt() > 0)
            displayState.timeout = time(nullptr) + server.arg("timeout").toInt();

    } else if (server.hasArg("ip")) {
        appSettings.showIP = server.arg("ip") != "false";
        if (displayState.theme == 1) displayUpdate(1, true);
        settingsSave(appSettings);

    } else if (server.hasArg("sec")) {
        appSettings.showSec = server.arg("sec") != "false";
        if (displayState.theme == 1) displayUpdate(1, true);
        settingsSave(appSettings);

    } else if (server.hasArg("weather")) {
        appSettings.showWeather = server.arg("weather") != "false";
        if (displayState.theme == 1) displayUpdate(1, true);
        settingsSave(appSettings);

    } else if (server.hasArg("tz")) {
        strncpy(appSettings.tz, server.arg("tz").c_str(), sizeof(appSettings.tz) - 1);
        appSettings.tz[sizeof(appSettings.tz) - 1] = '\0';
        setenv("TZ", appSettings.tz, 1); tzset();
        if (displayState.theme == 1) displayUpdate(1, true);
        settingsSave(appSettings);

    } else if (server.hasArg("owmLoc") && server.hasArg("owmKey")) {
        strncpy(appSettings.owmLocation, server.arg("owmLoc").c_str(), sizeof(appSettings.owmLocation) - 1);
        appSettings.owmLocation[sizeof(appSettings.owmLocation) - 1] = '\0';
        strncpy(appSettings.owmApiKey, server.arg("owmKey").c_str(), sizeof(appSettings.owmApiKey) - 1);
        appSettings.owmApiKey[sizeof(appSettings.owmApiKey) - 1] = '\0';
        if (displayState.theme == 1) displayUpdate(1, true);
        settingsSave(appSettings);

    } else {
        server.send(400, "text/plain", "No action");
        return;
    }
    server.send(200, "text/plain", "OK");
}

void handleTest() { displayTest(); server.send(200, "text/plain", "OK"); }

// =====================================================================
//  ЗАГРУЗКА ФАЙЛОВ
//  FIX: в оригинале write() вызывался ДВАЖДЫ при ошибке (один раз в
//  условии, второй — в logPrintf), что приводило к двойной записи.
// =====================================================================
static File uploadFile;

void handleFileUpload() {
    String dir = server.hasArg("dir") ? server.arg("dir") : "/";
    if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);

    const HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filepath = dir + upload.filename;
        uploadFile = LittleFS.open(filepath, "w");
        if (!uploadFile) logPrint("Failed to open file for writing!");

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            size_t written = uploadFile.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize)
                logPrintf("Only %u of %u bytes written!", written, upload.currentSize);
        }

    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            logPrintf("File uploaded: %u bytes", upload.totalSize);
        }
    }
}

void handleUploadDone() { server.send(200, "text/plain", "OK"); }

void handleDelete() {
    if (!server.hasArg("file")) { server.send(400, "text/plain", "Missing file parameter"); return; }
    char imagePath[DISPLAY_IMG_PATH_BUFFER_SIZE];
    urlDecode(server.arg("file").c_str(), imagePath, sizeof(imagePath));
    // FIX оригинала: logPrintf("File deleted", imagePath) — аргумент не использовался
    if (LittleFS.remove(imagePath)) {
        logPrintf("File deleted: %s", imagePath);
        server.send(200, "text/plain", "Deleted");
    } else {
        server.send(404, "text/plain", "Not found");
    }
}

// FIX: исправлен битый HTML (<tr></td> → <tr><td>, убран лишний </tr>)
void streamDirRecursiveHtml(const char *dirname) {
    File root = LittleFS.open(dirname, "r");
    if (!root || !root.isDirectory()) return;

    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            size_t len = strlen(file.fullName()) + 2;
            char childPath[len];
            snprintf(childPath, len, "/%s", file.fullName());
            streamDirRecursiveHtml(childPath);
        } else {
            const char *fileName = file.name();
            size_t      fileSize = file.size();
            String      nameLower = String(fileName);
            nameLower.toLowerCase();

            server.sendContent(F("<tr><td>"));   // FIX: было <tr></td>
            if (strcmp(displayState.image, file.fullName()) == 0)
                server.sendContent(F("&#x2714; "));
            server.sendContent(F("<a href='"));
            server.sendContent(dirname);
            server.sendContent(F("/"));
            server.sendContent(fileName);
            server.sendContent(F("'>"));
            server.sendContent(fileName);
            server.sendContent(F("</a></td><td class='size'>"));
            server.sendContent(String(fileSize));
            server.sendContent(F("</td><td><div class='button-group'>"));
            server.sendContent(F("<button class='button' onclick=\"deleteImage('"));
            server.sendContent(dirname);
            server.sendContent(F("/"));
            server.sendContent(fileName);
            server.sendContent(F("')\">DEL</button>"));
            if (nameLower.endsWith(F(".jpg"))) {
                server.sendContent(F("<button class='button' onclick=\"displayImage('"));
                server.sendContent(dirname);
                server.sendContent(F("/"));
                server.sendContent(fileName);
                server.sendContent(F("')\">SET</button>"));
            }
            server.sendContent(F("</div></td></tr>\n"));   // FIX: было </div></tr></tr>
        }
        file = root.openNextFile();
    }
}

void handleFileList() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent(F("<table><thead><tr><th>Path</th><th>Size</th><th>Actions</th></tr></thead><tbody>\n"));
    streamDirRecursiveHtml(server.hasArg("dir") ? server.arg("dir").c_str() : "/");
    server.sendContent(F("</tbody></table>\n"));
    server.sendContent("");
}

void handleFactoryReset() {
    server.send(200, "text/plain", "Factory Reset triggered. Clearing data and restarting...");
    delay(100);
    factoryReset();
}

void handleOTAForm() {
    server.sendHeader(F("Content-Encoding"), F("gzip"));
    server.sendHeader(F("Cache-Control"), F("max-age=600"));
    server.send_P(200, "text/html", (const char *)src_generated_ota_html_gz, src_generated_ota_html_gz_len);
}

void handleOTAUpload() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        showMessage("OTA обновление...");
        uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSpace)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) { Update.printError(Serial); showMessage("OTA ошибка!"); }
    }
}

void handleOTADone() {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "OK - Rebooting..." : "FAIL");
    if (ok) { showMessage("Готово!\nПерезагрузка...", 2); delay(2000); ESP.restart(); }
}

void handleLog() { server.send(200, "text/plain", logGetAll()); }

void handleWiFiScan() {
    int          num = WiFi.scanNetworks(false, true);
    JsonDocument doc;
    JsonArray    arr = doc.to<JsonArray>();
    for (int i = 0; i < num; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = WiFi.SSID(i);
        obj["rssi"] = WiFi.RSSI(i);
    }
    WiFi.scanDelete();
    sendJson(doc);
}

void handleWiFiConnect() {
    if (!server.hasArg("ssid")) { server.send(400, "text/plain", "Missing SSID"); return; }
    String ssid     = server.arg("ssid");
    String password = server.hasArg("password") ? server.arg("password") : "";

    server.send(200, "text/plain", "Connecting...");
    delay(100);

    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());

    // FIX: yield() внутри цикла ожидания для сброса WDT
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        yield();
    }

    showMessage(WiFi.status() == WL_CONNECTED ? "Готово!\nПерезагрузка..." : "Ошибка :(\nПерезагрузка...");
    delay(2000);
    ESP.restart();
}

void handleStatic() {
    String path = server.uri();
    if (!LittleFS.exists(path)) { server.send(404, "text/plain", "File not found"); return; }
    File file = LittleFS.open(path, "r");
    if (!file)                   { server.send(500, "text/plain", "Failed to open file"); return; }
    server.streamFile(file, path.endsWith(F(".jpg")) ? "image/jpeg" : "application/octet-stream");
    file.close();
}

void handleRoot() {
    server.sendHeader(F("Content-Encoding"), F("gzip"));
    server.sendHeader(F("Cache-Control"), F("max-age=600"));
    server.send_P(200, "text/html", (const char *)src_generated_index_html_gz, src_generated_index_html_gz_len);
}

void webserverInit() {
    server.on("/",             HTTP_GET,  handleRoot);
    server.on("/app.json",     HTTP_GET,  handleAppJson);
    server.on("/space.json",   HTTP_GET,  handleSpaceJson);
    server.on("/memory.json",  HTTP_GET,  handleMemoryJson);
    server.on("/brt.json",     HTTP_GET,  handleBrtJson);
    server.on("/v.json",       HTTP_GET,  handleVersionJson);
    server.on("/message.json", HTTP_GET,  handleMessageJson);
    server.on("/note.json",    HTTP_GET,  handleNoteJson);
    server.on("/filelist",     HTTP_GET,  handleFileList);
    server.on("/delete",       HTTP_GET,  handleDelete);
    server.on("/set",          HTTP_GET,  handleSet);
    server.on("/test",         HTTP_GET,  handleTest);
    server.on("/log",          HTTP_GET,  handleLog);
    server.on("/factoryreset", HTTP_GET,  handleFactoryReset);
    server.on("/scan",         HTTP_GET,  handleWiFiScan);
    server.on("/connect",      HTTP_GET,  handleWiFiConnect);
    server.on("/doUpload",     HTTP_POST, handleUploadDone, handleFileUpload);
    server.on("/update",       HTTP_GET,  handleOTAForm);
    server.on("/update",       HTTP_POST, handleOTADone, handleOTAUpload);
    server.onNotFound(handleStatic);
    server.begin();
    Serial.println("Web server started");
}

// =====================================================================
//  WIFI & СИСТЕМА
// =====================================================================
void factoryReset() {
    showMessage("Сброс\nнастроек...");
    WiFi.disconnect(true); yield();
    ESP.eraseConfig();     yield();
    settingsReset(appSettings);
    powerCycleCounterReset();
    LittleFS.format();     yield();
    Serial.println("Factory reset complete. Rebooting...");
    showMessage("Готово!\nПерезагрузка...", 2);
    delay(2000);
    ESP.restart();
}

bool tryConnectWiFi(int maxAttempts) {
    Serial.printf("Attempting WiFi connection (max %d attempts)...\n", maxAttempts);
    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        Serial.printf("WiFi attempt %d/%d\n", attempt, maxAttempts);
        WiFi.mode(WIFI_STA);
        WiFi.begin();
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECTION_TIMEOUT) {
            delay(1000); yield();
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("WiFi associated, waiting for IP...");
            unsigned long t1 = millis();
            while (WiFi.localIP() == IPAddress(0,0,0,0) && millis() - t1 < 10000) {
                delay(1000); yield();
            }
        }
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0)) {
            Serial.println("WiFi connected!");
            showMessage(WiFi.localIP().toString());
            delay(2000);
            return true;
        }
        if (attempt < maxAttempts) {
            unsigned long retryMs = (unsigned long)WIFI_RETRY_DELAY_MS << (attempt - 1);
            if (retryMs > 30000) retryMs = 30000;
            Serial.printf("Retry in %lu ms...\n", retryMs);
            delay(retryMs);
        }
    }
    return false;
}

void startAPMode() {
    Serial.println("Entering failsafe AP mode");
    WiFi.disconnect(true); yield();
    WiFi.mode(WIFI_AP);    yield();
    WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD);
    strncpy(displayState.ipInfo, WiFi.softAPIP().toString().c_str(), sizeof(displayState.ipInfo) - 1);
    displayState.ipInfo[sizeof(displayState.ipInfo) - 1] = '\0';
    displayUpdate(-1, true);
}

void setupWiFi() {
    Serial.println("Starting WiFi Setup...");
    String ssid = WiFi.SSID();
    if (ssid.isEmpty()) {
        Serial.println("No saved WiFi credentials - going directly to AP mode");
        startAPMode();
    } else {
        Serial.println("Attempting to connect with saved credentials...");
        if (tryConnectWiFi(WIFI_RETRY_ATTEMPTS))
            Serial.println("Connected successfully!");
        else {
            Serial.println("WiFi connection failed - entering AP mode");
            startAPMode();
        }
    }
    Serial.println("WiFi setup completed");
}

void setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([] {
        String type = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
        Serial.println("OTA Start: " + type);
        showMessage("OTA обновление...", 0, -15);
        tft.drawRect(20, 120, 200, 20, TFT_WHITE);
        tft.fillRect(22, 122, 196, 16, TFT_BLACK);
    });

    ArduinoOTA.onEnd([] {
        Serial.println("OTA Complete");
        showMessage("Готово!\nПерезагрузка...", 2);
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        int percent = (int)(progress * 100 / total);
        static int lastPercent = -1;
        if (percent != lastPercent) {
            tft.fillRect(22, 122, percent * 196 / 100, 16, TFT_BLUE);
            lastPercent = percent;
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA Error[%u]\n", error);
        showMessage("OTA ошибка!");
    });

    ArduinoOTA.begin();
    Serial.println("OTA ready");
}

void setupFilesystem() {
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed. Formatting...");
        showMessage("Форматирование FS...", 2);
        if (LittleFS.format()) {
            Serial.println("LittleFS formatted. Mounting...");
            delay(1000);
            if (!LittleFS.begin()) {
                Serial.println("Still cannot mount after format!");
                showMessage("Ошибка FS!\nВеб недоступен", 3);
                return;
            }
        } else {
            Serial.println("Format failed!");
            showMessage("Ошибка формата!", 2);
            return;
        }
    }
    Serial.println("LittleFS ready");
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(100);

    loggerInit();
    logPrint("Starting...");
    logPrintf("Firmware Version: %d", FIRMWARE_VERSION);

    settingsInit();
    powerCycleCounterIncrement();

    displayInit();
    displaySetBrightness(DEFAULT_BRIGHTNESS);
    showMessage("Запуск...");

    settingsLoad(appSettings);

    if (powerCycleCounterCheckReset()) {
        Serial.println("USER RESET: 5 quick power cycles detected!");
        factoryReset();
        return;
    }

    buttonInit();
    displaySetBrightness(appSettings.brightness);

    setupFilesystem();
    setupWiFi();
    webserverInit();
    setupOTA();

    strncpy(displayState.ipInfo, WiFi.localIP().toString().c_str(), sizeof(displayState.ipInfo) - 1);
    displayState.ipInfo[sizeof(displayState.ipInfo) - 1] = '\0';

    if (displayState.theme >= 0) {
        configTzTime(appSettings.tz, NTP_SERVER);
        yield();
        displayUpdate(1, true);
        lastDisplayUpdate = millis();
    }

    logPrint("Setup complete");
}

void loop() {
    // Сбрасываем счётчик перезагрузок после успешного старта
    if (!powerCycleCounterCleared && millis() > 10000) {
        powerCycleCounterReset();
        powerCycleCounterCleared = true;
        Serial.println("Power cycle counter cleared after successful boot");
    }

    if (displayState.theme >= 0) {
        ButtonPress bp = buttonUpdate();
        if (bp == BUTTON_SHORT) { displayCycleNextPage(); return; }
        if (bp == BUTTON_LONG)  { displayToggleBacklight(); return; }
    }

    unsigned long now = millis();
    ArduinoOTA.handle();
    webserverHandle();

    if (now - lastDisplayUpdate > DISPLAY_UPDATE_INTERVAL) {
        lastDisplayUpdate = now;
        displayUpdate(0, false);
        if (lastWeatherUpdate == 0 || now - lastWeatherUpdate > WEATHER_UPDATE_INTERVAL) {
            if (weatherUpdateTask()) lastWeatherUpdate = now;
        }
    }

    yield();
}

void webserverHandle() { server.handleClient(); }
