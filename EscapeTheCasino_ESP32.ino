#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <ESP_I2S.h>
#include <cstring>

// Escape the Casino - ESP32-S3 console edition
// Arduino-ESP32 3.x, Adafruit GFX, Adafruit ILI9341

// ---------------- Hardware pins ----------------
constexpr int TFT_CS = 10;
constexpr int TFT_DC = 9;
constexpr int TFT_MOSI = 11;
constexpr int TFT_SCK = 12;
constexpr int TFT_RST = 8;
constexpr int SD_MISO = 13;
constexpr int SD_CS = 14;

constexpr int BUTTON_X = 38;
constexpr int BUTTON_Y = 39;
constexpr int BUTTON_A = 40;
constexpr int BUTTON_B = 41;
constexpr int JOYSTICK_X = 4;
constexpr int JOYSTICK_Y = 5;
constexpr int JOYSTICK_SW = 6;

constexpr int I2S_DIN = 16;
constexpr int I2S_BCLK = 17;
constexpr int I2S_LRC = 18;

constexpr int CENTER_X = 1861;
constexpr int CENTER_Y = 1837;
constexpr int DEAD_ZONE = 600;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
I2SClass audio;

constexpr uint16_t COLOR_ORANGE = 0xFD20;
constexpr uint16_t ICON_TRANSPARENT = 0xF81F;  // Magenta transparency key.

bool sdReady = false;
bool audioReady = false;
volatile bool musicEnabled = true;
volatile bool sfxEnabled = true;
volatile int masterVolume = 100;

// ---------------- Audio ----------------
constexpr uint32_t SAMPLE_RATE = 22050;
constexpr int AUDIO_FRAMES = 64;
int16_t audioBuffer[AUDIO_FRAMES * 2];

const char* MUSIC_MENU = "/assets/music/menu.wav";
const char* MUSIC_COMBAT = "/assets/music/combat.wav";
const char* MUSIC_TENSION = "/assets/music/tension.wav";
const char* MUSIC_STORE = "/assets/music/store.wav";
const char* MUSIC_BOSS = "/assets/music/boss.wav";

const char* SFX_SPEND = "/assets/sfx/spendCoin.wav";
const char* SFX_PICKUP = "/assets/sfx/pickupCoin.wav";
const char* SFX_HURT = "/assets/sfx/hurt.wav";
const char* SFX_HIT = "/assets/sfx/hit.wav";
const char* SFX_EXPLOSION = "/assets/sfx/explosion.wav";
const char* SFX_CRITICAL_WIN = "/assets/sfx/criticalWin.wav";
const char* SFX_CRITICAL_ROLL = "/assets/sfx/criticalRoll.wav";
const char* IMAGE_TITLE = "/assets/images/title.bmp";
const char* IMAGE_GAME_OVER = "/assets/images/gameover.bmp";
const char* IMAGE_VICTORY = "/assets/images/victory.bmp";

constexpr int STORE_ICON_SIZE = 20;
constexpr int STORE_ICON_PIXELS = STORE_ICON_SIZE * STORE_ICON_SIZE;
const char* STORE_ICON_PATHS[] = {
  "/assets/icons/store/silver_die.rgb565",
  "/assets/icons/store/gold_die.rgb565",
  "/assets/icons/store/double_dice.rgb565",
  "/assets/icons/store/green_potion.rgb565",
  "/assets/icons/store/armor.rgb565"
};
uint16_t storeIcons[5][STORE_ICON_PIXELS];
bool storeIconLoaded[5] = {false, false, false, false, false};
uint16_t* titleImageCache = nullptr;

SemaphoreHandle_t sdMutex = nullptr;
portMUX_TYPE audioCommandMux = portMUX_INITIALIZER_UNLOCKED;
char requestedMusic[64] = "";
char requestedSfx[64] = "";
volatile uint32_t musicCommand = 0;
volatile uint32_t sfxCommand = 0;
volatile uint32_t completedSfxCommand = 0;

struct WavStream {
  File file;
  uint32_t dataStart = 0;
  uint32_t dataSize = 0;
  uint32_t bytesRead = 0;
  uint32_t sampleRate = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  bool valid = false;
};

// Explicit declarations keep Arduino's .ino preprocessor from moving these
// prototypes above the WavStream type.
uint16_t readLE16(File& file);
uint32_t readLE32(File& file);
bool openWav(WavStream& stream, const char* path);
int16_t readSourceFrame(WavStream& stream, bool& ended);
int16_t readOutputSample(WavStream& stream, bool loop, bool& ended);
void audioTask(void* parameter);

uint16_t readLE16(File& file) {
  uint16_t value = file.read();
  value |= static_cast<uint16_t>(file.read()) << 8;
  return value;
}

uint32_t readLE32(File& file) {
  uint32_t value = file.read();
  value |= static_cast<uint32_t>(file.read()) << 8;
  value |= static_cast<uint32_t>(file.read()) << 16;
  value |= static_cast<uint32_t>(file.read()) << 24;
  return value;
}

bool openWav(WavStream& stream, const char* path) {
  if (stream.file) stream.file.close();
  stream = WavStream();
  stream.file = SD.open(path, FILE_READ);
  if (!stream.file) return false;

  char id[5] = {0};
  stream.file.readBytes(id, 4);
  if (strcmp(id, "RIFF") != 0) { stream.file.close(); return false; }
  readLE32(stream.file);
  stream.file.readBytes(id, 4);
  if (strcmp(id, "WAVE") != 0) { stream.file.close(); return false; }

  bool foundFormat = false;
  while (stream.file.available()) {
    stream.file.readBytes(id, 4);
    const uint32_t chunkSize = readLE32(stream.file);
    const uint32_t nextChunk = stream.file.position() + chunkSize + (chunkSize & 1);
    if (strcmp(id, "fmt ") == 0) {
      const uint16_t format = readLE16(stream.file);
      stream.channels = readLE16(stream.file);
      stream.sampleRate = readLE32(stream.file);
      readLE32(stream.file); readLE16(stream.file);
      stream.bits = readLE16(stream.file);
      foundFormat = format == 1;
    } else if (strcmp(id, "data") == 0 && foundFormat) {
      stream.dataStart = stream.file.position();
      stream.dataSize = chunkSize;
      stream.bytesRead = 0;
      stream.valid = (stream.channels == 1 || stream.channels == 2) &&
                     (stream.bits == 8 || stream.bits == 16) &&
                     (stream.sampleRate == 22050 || stream.sampleRate == 44100);
      return stream.valid;
    }
    stream.file.seek(nextChunk);
  }
  stream.file.close();
  return false;
}

int16_t readSourceFrame(WavStream& stream, bool& ended) {
  ended = false;
  const int bytesPerSample = stream.bits / 8;
  const int bytesPerFrame = bytesPerSample * stream.channels;
  if (!stream.valid || stream.bytesRead + bytesPerFrame > stream.dataSize) {
    ended = true;
    return 0;
  }

  int32_t sum = 0;
  for (int channel = 0; channel < stream.channels; ++channel) {
    if (stream.bits == 8)
      sum += (static_cast<int>(stream.file.read()) - 128) << 8;
    else
      sum += static_cast<int16_t>(readLE16(stream.file));
  }
  stream.bytesRead += bytesPerFrame;
  return static_cast<int16_t>(sum / stream.channels);
}

int16_t readOutputSample(WavStream& stream, bool loop, bool& ended) {
  bool firstEnded = false;
  int16_t first = readSourceFrame(stream, firstEnded);
  if (firstEnded && loop && stream.valid) {
    stream.file.seek(stream.dataStart);
    stream.bytesRead = 0;
    first = readSourceFrame(stream, firstEnded);
  }
  if (firstEnded) { ended = true; return 0; }

  if (stream.sampleRate == 44100) {
    bool secondEnded = false;
    const int16_t second = readSourceFrame(stream, secondEnded);
    if (!secondEnded) first = static_cast<int16_t>((static_cast<int32_t>(first) + second) / 2);
  }
  ended = false;
  return first;
}

void startMusic(const char* path) {
  if (!audioReady) return;
  const char* safePath = path ? path : "";
  portENTER_CRITICAL(&audioCommandMux);
  // Re-requesting the current track should not rewind it. This keeps music
  // continuous across consecutive screens that share the same soundtrack.
  if (strcmp(requestedMusic, safePath) == 0) {
    portEXIT_CRITICAL(&audioCommandMux);
    return;
  }
  strncpy(requestedMusic, safePath, sizeof(requestedMusic) - 1);
  requestedMusic[sizeof(requestedMusic) - 1] = '\0';
  ++musicCommand;
  portEXIT_CRITICAL(&audioCommandMux);
}

void stopMusic() { startMusic(""); }

uint32_t requestSfx(const char* path) {
  if (!audioReady || !sfxEnabled) return 0;
  portENTER_CRITICAL(&audioCommandMux);
  strncpy(requestedSfx, path, sizeof(requestedSfx) - 1);
  requestedSfx[sizeof(requestedSfx) - 1] = '\0';
  const uint32_t command = ++sfxCommand;
  portEXIT_CRITICAL(&audioCommandMux);
  return command;
}

void playSfxBlocking(const char* path) {
  const uint32_t command = requestSfx(path);
  if (command == 0) return;
  while (completedSfxCommand < command) delay(2);
}

void audioTask(void*) {
  WavStream music;
  WavStream sfx;
  uint32_t handledMusic = 0;
  uint32_t handledSfx = 0;

  while (true) {
    char newMusic[64] = "";
    char newSfx[64] = "";
    uint32_t observedMusic;
    uint32_t observedSfx;
    portENTER_CRITICAL(&audioCommandMux);
    observedMusic = musicCommand;
    observedSfx = sfxCommand;
    if (observedMusic != handledMusic) strcpy(newMusic, requestedMusic);
    if (observedSfx != handledSfx) strcpy(newSfx, requestedSfx);
    portEXIT_CRITICAL(&audioCommandMux);

    xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (observedMusic != handledMusic) {
      if (music.file) music.file.close();
      music = WavStream();
      if (newMusic[0] != '\0') openWav(music, newMusic);
      handledMusic = observedMusic;
    }
    if (observedSfx != handledSfx) {
      if (sfx.file) sfx.file.close();
      sfx = WavStream();
      if (!openWav(sfx, newSfx)) completedSfxCommand = observedSfx;
      handledSfx = observedSfx;
    }

    for (int i = 0; i < AUDIO_FRAMES; ++i) {
      bool musicEnded = false;
      bool sfxEnded = false;
      const int16_t musicSample = music.valid ? readOutputSample(music, true, musicEnded) : 0;
      const int16_t sfxSample = sfx.valid ? readOutputSample(sfx, false, sfxEnded) : 0;
      if (sfx.valid && sfxEnded) {
        sfx.file.close();
        sfx.valid = false;
        completedSfxCommand = handledSfx;
      }
      int32_t mixed = 0;
      if (musicEnabled)
        mixed += static_cast<int32_t>(musicSample) * 35 / 100;
      if (sfxEnabled)
        mixed += static_cast<int32_t>(sfxSample) * 85 / 100;
      mixed = mixed * masterVolume / 100;
      mixed = constrain(mixed, -32768, 32767);
      audioBuffer[i * 2] = static_cast<int16_t>(mixed);
      audioBuffer[i * 2 + 1] = static_cast<int16_t>(mixed);
    }
    xSemaphoreGive(sdMutex);
    audio.write(audioBuffer, sizeof(audioBuffer));
  }
}

void soundMove() {}
void soundSelect() {}
void soundHit() { playSfxBlocking(SFX_HIT); }
void soundCritical() { playSfxBlocking(SFX_CRITICAL_WIN); }
void soundHeal() {}
void soundPurchase() { playSfxBlocking(SFX_SPEND); }
void soundWin() { playSfxBlocking(SFX_PICKUP); }
void soundLose() { playSfxBlocking(SFX_HURT); }

// ---------------- Input ----------------
enum Direction { CENTER, UP, DOWN, LEFT, RIGHT };

// Explicit declaration prevents Arduino's .ino preprocessor from placing an
// automatically generated prototype before the Direction type above.
Direction readDirection();

Direction readDirection() {
  const int dx = analogRead(JOYSTICK_X) - CENTER_X;
  const int dy = analogRead(JOYSTICK_Y) - CENTER_Y;
  if (abs(dx) < DEAD_ZONE && abs(dy) < DEAD_ZONE) return CENTER;
  if (abs(dx) > abs(dy)) return dx < 0 ? LEFT : RIGHT;
  return dy < 0 ? UP : DOWN;
}

bool confirmDown() {
  return digitalRead(BUTTON_A) == LOW || digitalRead(JOYSTICK_SW) == LOW;
}

void waitForRelease() {
  while (readDirection() != CENTER || confirmDown() ||
         digitalRead(BUTTON_B) == LOW || digitalRead(BUTTON_X) == LOW ||
         digitalRead(BUTTON_Y) == LOW) {
    delay(10);
  }
  delay(35);
}

int chooseMenu(const char* title, const char* const items[], int count,
               int initial = 0, const char* footer = "A: Select") {
  int selected = constrain(initial, 0, count - 1);
  waitForRelease();

  while (true) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextWrap(true);
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(2);
    tft.setCursor(10, 8);
    tft.println(title);
    tft.drawFastHLine(10, 30, 300, ILI9341_CYAN);

    const int itemHeight = min(36, 170 / count);
    const int top = 42;
    for (int i = 0; i < count; ++i) {
      const int y = top + i * itemHeight;
      if (i == selected) {
        tft.fillRoundRect(10, y, 300, itemHeight - 4, 4, ILI9341_CYAN);
        tft.setTextColor(ILI9341_BLACK);
      } else {
        tft.setTextColor(ILI9341_WHITE);
      }
      tft.setTextSize(count > 5 ? 1 : 2);
      tft.setCursor(18, y + (count > 5 ? 7 : 6));
      tft.print(items[i]);
    }

    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(1);
    tft.setCursor(10, 226);
    tft.print(footer);

    while (true) {
      Direction d = readDirection();
      if (d == UP || d == DOWN) {
        selected += d == UP ? -1 : 1;
        if (selected < 0) selected = count - 1;
        if (selected >= count) selected = 0;
        soundMove();
        waitForRelease();
        break;
      }
      if (confirmDown()) {
        soundSelect();
        waitForRelease();
        return selected;
      }
      delay(10);
    }
  }
}

// Adafruit_GFX's normal wrapping can divide a word at the screen edge. This
// printer moves the entire word to the next line when it will not fit.
void printWrapped(const String& text, int x, int y, int maxWidth,
                  uint8_t textSize, uint16_t color, int maxLines = 20) {
  const int charWidth = 6 * textSize;
  const int lineHeight = 8 * textSize + 2;
  const int maxChars = max(1, maxWidth / charWidth);
  int line = 0;
  int column = 0;
  String word;

  tft.setTextWrap(false);
  tft.setTextSize(textSize);
  tft.setTextColor(color);
  tft.setCursor(x, y);

  auto newLine = [&]() {
    ++line;
    column = 0;
    if (line < maxLines) tft.setCursor(x, y + line * lineHeight);
  };

  for (unsigned int i = 0; i <= text.length(); ++i) {
    const char c = i < text.length() ? text[i] : ' ';
    if (c != ' ' && c != '\n') {
      word += c;
      continue;
    }

    if (word.length() > 0 && line < maxLines) {
      if (column > 0 && column + 1 + static_cast<int>(word.length()) > maxChars)
        newLine();
      if (line >= maxLines) break;
      if (column > 0) { tft.print(' '); ++column; }
      tft.print(word);
      column += word.length();
      word = "";
    }

    if (c == '\n' && line < maxLines) newLine();
  }
  tft.setTextWrap(true);
}

void messagePage(const char* title, const String& text,
                 uint16_t color = ILI9341_WHITE) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextWrap(true);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.println(title);
  tft.drawFastHLine(10, 30, 300, ILI9341_CYAN);
  printWrapped(text, 10, 43, 300, 2, color, 9);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(10, 226);
  tft.print("A or joystick button: Continue");
  waitForRelease();
  while (!confirmDown()) delay(10);
  soundSelect();
  waitForRelease();
}

bool drawBmp24At(const char* path, int16_t screenX, int16_t screenY) {
  if (!sdReady) return false;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File file = SD.open(path, FILE_READ);
  if (!file) { xSemaphoreGive(sdMutex); return false; }

  if (readLE16(file) != 0x4D42) {
    file.close(); xSemaphoreGive(sdMutex); return false;
  }
  readLE32(file); readLE32(file);
  const uint32_t pixelOffset = readLE32(file);
  readLE32(file);
  const int32_t width = static_cast<int32_t>(readLE32(file));
  const int32_t signedHeight = static_cast<int32_t>(readLE32(file));
  readLE16(file);
  const uint16_t depth = readLE16(file);
  const uint32_t compression = readLE32(file);
  const int32_t height = abs(signedHeight);

  if (width <= 0 || height <= 0 || width > 320 || height > 240 ||
      screenX < 0 || screenY < 0 || screenX + width > 320 ||
      screenY + height > 240 || depth != 24 || compression != 0) {
    file.close(); xSemaphoreGive(sdMutex); return false;
  }

  uint16_t* pixels = static_cast<uint16_t*>(ps_malloc(width * height * sizeof(uint16_t)));
  if (!pixels) pixels = static_cast<uint16_t*>(malloc(width * height * sizeof(uint16_t)));
  if (!pixels) { file.close(); xSemaphoreGive(sdMutex); return false; }

  const uint32_t rowSize = (width * 3 + 3) & ~3;
  uint8_t row[320 * 3];
  bool success = true;
  for (int y = 0; y < height; ++y) {
    const int sourceY = signedHeight > 0 ? height - 1 - y : y;
    file.seek(pixelOffset + sourceY * rowSize);
    if (file.read(row, width * 3) != width * 3) { success = false; break; }
    for (int x = 0; x < width; ++x) {
      const uint8_t blue = row[x * 3];
      const uint8_t green = row[x * 3 + 1];
      const uint8_t red = row[x * 3 + 2];
      pixels[y * width + x] = tft.color565(red, green, blue);
    }
  }
  file.close();
  xSemaphoreGive(sdMutex);

  if (success) tft.drawRGBBitmap(screenX, screenY, pixels, width, height);
  free(pixels);
  return success;
}

bool drawBmp24(const char* path) {
  return drawBmp24At(path, 0, 0);
}

bool cacheTitleImage() {
  if (!sdReady || titleImageCache != nullptr) return titleImageCache != nullptr;

  uint16_t* cache = static_cast<uint16_t*>(
      ps_malloc(320 * 240 * sizeof(uint16_t)));
  if (!cache) return false;

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File file = SD.open(IMAGE_TITLE, FILE_READ);
  bool success = file;

  uint32_t pixelOffset = 0;
  int32_t signedHeight = 0;
  if (success && readLE16(file) == 0x4D42) {
    readLE32(file);  // File size.
    readLE32(file);  // Reserved fields.
    pixelOffset = readLE32(file);
    readLE32(file);  // DIB header size.
    const int32_t width = static_cast<int32_t>(readLE32(file));
    signedHeight = static_cast<int32_t>(readLE32(file));
    readLE16(file);  // Color planes.
    const uint16_t depth = readLE16(file);
    const uint32_t compression = readLE32(file);
    success = width == 320 && abs(signedHeight) == 240 &&
              depth == 24 && compression == 0;
  } else {
    success = false;
  }

  uint8_t row[320 * 3];
  if (success) {
    constexpr uint32_t rowSize = 320 * 3;
    for (int y = 0; y < 240; ++y) {
      const int sourceY = signedHeight > 0 ? 239 - y : y;
      file.seek(pixelOffset + sourceY * rowSize);
      if (file.read(row, sizeof(row)) != sizeof(row)) {
        success = false;
        break;
      }
      for (int x = 0; x < 320; ++x) {
        cache[y * 320 + x] = tft.color565(
            row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
      }
    }
  }

  if (file) file.close();
  xSemaphoreGive(sdMutex);

  if (!success) {
    free(cache);
    return false;
  }

  titleImageCache = cache;
  return true;
}

bool drawTitleImage() {
  if (titleImageCache != nullptr) {
    tft.drawRGBBitmap(0, 0, titleImageCache, 320, 240);
    return true;
  }
  return drawBmp24(IMAGE_TITLE);
}

bool loadRgb565Icon(const char* path, uint16_t* destination) {
  if (!sdReady) return false;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File file = SD.open(path, FILE_READ);
  const size_t expectedBytes = STORE_ICON_PIXELS * sizeof(uint16_t);
  const bool valid = file && file.size() == expectedBytes &&
                     file.read(reinterpret_cast<uint8_t*>(destination),
                               expectedBytes) == expectedBytes;
  if (file) file.close();
  xSemaphoreGive(sdMutex);
  return valid;
}

void loadStoreIcons() {
  for (int i = 0; i < 5; ++i)
    storeIconLoaded[i] = loadRgb565Icon(STORE_ICON_PATHS[i], storeIcons[i]);
}

void drawStoreIcon(int index, int16_t x, int16_t y) {
  if (index < 0 || index >= 5 || !storeIconLoaded[index]) return;
  for (int py = 0; py < STORE_ICON_SIZE; ++py) {
    for (int px = 0; px < STORE_ICON_SIZE; ++px) {
      const uint16_t color = storeIcons[index][py * STORE_ICON_SIZE + px];
      if (color != ICON_TRANSPARENT) tft.drawPixel(x + px, y + py, color);
    }
  }
}

void drawStoreIconScaled(int index, int16_t x, int16_t y, int scale) {
  if (index < 0 || index >= 5 || !storeIconLoaded[index]) return;
  for (int py = 0; py < STORE_ICON_SIZE; ++py) {
    for (int px = 0; px < STORE_ICON_SIZE; ++px) {
      const uint16_t color = storeIcons[index][py * STORE_ICON_SIZE + px];
      if (color != ICON_TRANSPARENT)
        tft.fillRect(x + px * scale, y + py * scale, scale, scale, color);
    }
  }
}

void imageMessage(const char* path, const String& footer, uint16_t color) {
  if (!drawBmp24(path)) {
    messagePage("ASSET ERROR", "Could not display " + String(path), ILI9341_RED);
    return;
  }
  tft.fillRect(0, 220, 320, 20, ILI9341_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(footer, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(max(2, (320 - static_cast<int>(w)) / 2), 226);
  tft.print(footer);
  waitForRelease();
  while (!confirmDown()) delay(10);
  waitForRelease();
}

// ---------------- Game data ----------------
struct Player {
  int maxLife = 25;
  int life = 25;
  int luck = 1;
  int coins = 0;
  int tokens = 0;
  int diceBonus = 0;
  bool armor = false;

  bool alive() const { return life > 0; }
  int tokenLimit() const { return armor ? 3 : 2; }
  void damage(int amount) { life = max(0, life - amount); }
  void heal(int amount) { life = min(maxLife, life + amount); }
};

struct Enemy {
  const char* name;
  const char* description;
  int life;
  int luck;
  int difficulty;
  int reward;
};

struct StageData {
  const char* name;
  const char* description;
  Enemy first;
  Enemy second;
  bool boss;
};

// These declarations must appear after Enemy is defined. Without them, the
// Arduino .ino preprocessor can generate prototypes before the Enemy struct.
void drawBattleStatus(const Enemy& enemy);
Enemy chooseEnemy();
int chooseOpponentScreen(const StageData& stage);
int playerAttack(Enemy& enemy);
int criticalRoll(Enemy& enemy);
int chooseCriticalPrediction(const Enemy& enemy, int choices);
int chooseCombatAction(const Enemy& enemy);
bool battle(Enemy enemy);
void combatPage(const Enemy& enemy, const char* title, const String& text,
                uint16_t color);

const StageData STAGES[] = {
  {"THE PRISON", "Smoke, decay, and distant screams.",
   {"Jailer", "A sadistic jailer who toys with prisoners.", 25, 1, 1, 30},
   {"Torturer", "Silent, patient, and smiling.", 30, 2, 2, 35}, false},
  {"THE BACKROOM", "People in chains serve the entertainers.",
   {"The Magician", "A trickster with dice. Watch his hands.", 35, 2, 3, 40},
   {"The Ballerina", "Unpredictable and dangerous.", 40, 3, 4, 45}, false},
  {"THE BAR", "Wild beasts wait in cages for the losers.",
   {"The Drinker", "He drinks and somehow knows things.", 45, 3, 4, 50},
   {"The Mixer", "Never ask what goes into his drinks.", 50, 4, 5, 55}, false},
  {"THE HOTEL", "A windowless room slowly fills with gas.",
   {"The Plumber", "Almost nothing remains after his work.", 55, 4, 5, 65},
   {"The Maid", "Her favorite color is victim red.", 60, 5, 6, 75}, false},
  {"THE MANSION", "The House will accept only one survivor.",
   {"Lisa", "A compulsive gambler who will not fold.", 70, 5, 6, 80},
   {"Martin", "A man with nothing left to lose.", 80, 6, 7, 90}, false},
  {"THE MASK PARTY", "Tonight, the main performance is you.",
   {"Sergei", "He swallows knives and wants to share.", 90, 6, 7, 100},
   {"Ivan", "A contortionist who bends other people.", 100, 7, 8, 120}, false},
  {"THE SECRET SOCIETY", "Dark figures surround you in silence.",
   {"Casino Owner", "The architect of every debt and death.", 200, 9, 10, 150},
   {"Casino Owner", "The architect of every debt and death.", 200, 9, 10, 150}, true}
};

constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

Player player;
int currentStage = 0;
int harderEnemyChoices = 0;
bool storePending = false;
int attackHitCount = 1;

void resetGame() {
  player = Player();
  currentStage = 0;
  harderEnemyChoices = 0;
  storePending = false;
}

int diceRoll() { return random(1, 21) + player.diceBonus; }
int baseRoll() { return random(1, 21); }

// ---------------- SD save ----------------
const char* SAVE_PATH = "/assets/casino.sav";

bool sdFileExists(const char* path) {
  if (!sdReady) return false;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  const bool exists = SD.exists(path);
  xSemaphoreGive(sdMutex);
  return exists;
}

bool saveGame() {
  if (!sdReady) return false;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  SD.remove(SAVE_PATH);
  File file = SD.open(SAVE_PATH, FILE_WRITE);
  if (!file) { xSemaphoreGive(sdMutex); return false; }
  file.println("CASINO_SAVE_V2");
  file.println(currentStage);
  file.println(player.maxLife);
  file.println(player.life);
  file.println(player.luck);
  file.println(player.coins);
  file.println(player.tokens);
  file.println(player.diceBonus);
  file.println(player.armor ? 1 : 0);
  file.println(harderEnemyChoices);
  file.println(storePending ? 1 : 0);
  file.close();
  xSemaphoreGive(sdMutex);
  return true;
}

bool loadGame() {
  if (!sdFileExists(SAVE_PATH)) return false;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File file = SD.open(SAVE_PATH, FILE_READ);
  String version = file ? file.readStringUntil('\n') : "";
  const bool version1 = version.indexOf("CASINO_SAVE_V1") >= 0;
  const bool version2 = version.indexOf("CASINO_SAVE_V2") >= 0;
  if (!file || (!version1 && !version2)) {
    if (file) file.close();
    xSemaphoreGive(sdMutex);
    return false;
  }
  currentStage = file.parseInt();
  player.maxLife = file.parseInt();
  player.life = file.parseInt();
  player.luck = file.parseInt();
  player.coins = file.parseInt();
  player.tokens = file.parseInt();
  player.diceBonus = file.parseInt();
  player.armor = file.parseInt() != 0;
  harderEnemyChoices = file.parseInt();
  storePending = version2 ? file.parseInt() != 0 : false;
  file.close();
  xSemaphoreGive(sdMutex);
  return currentStage >= 0 && currentStage < STAGE_COUNT && player.life > 0;
}

void deleteSave() {
  if (!sdReady) return;
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (SD.exists(SAVE_PATH)) SD.remove(SAVE_PATH);
  xSemaphoreGive(sdMutex);
}

// ---------------- Story and status ----------------
void showIntro() {
  startMusic(MUSIC_TENSION);
  messagePage("THE DEBT",
              "You are trapped inside a casino.\n\nThe House owns your debt-and your life.");
  messagePage("THE RULES",
              "Fight through seven arenas.\n\nWin and leave. Lose... and you die.");
}

void showStage() {
  startMusic(currentStage == STAGE_COUNT - 1 ? MUSIC_BOSS : MUSIC_TENSION);
  const StageData& stage = STAGES[currentStage];
  String text = currentStage == STAGE_COUNT - 1 ? "FINAL STAGE\n\n" :
                "STAGE " + String(currentStage + 1) + "\n\n";
  text += stage.name;
  text += "\n\n";
  text += stage.description;
  messagePage("NEXT ARENA", text,
              stage.boss ? ILI9341_RED : ILI9341_WHITE);
}

void drawBattleStatus(const Enemy& enemy) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextWrap(true);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("YOU ");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(player.life); tft.print('/'); tft.print(player.maxLife);
  tft.setTextColor(ILI9341_CYAN);
  tft.print(" COINS ");
  tft.setTextColor(ILI9341_WHITE);
  tft.println(player.coins);
  tft.setCursor(8, 29);
  tft.setTextColor(ILI9341_RED);
  tft.print(enemy.name); tft.print("  HP "); tft.println(enemy.life);
  tft.drawFastHLine(8, 53, 304, ILI9341_CYAN);
}

int chooseOpponentScreen(const StageData& stage) {
  int selected = 0;
  waitForRelease();
  while (true) {
    const Enemy& shown = selected == 0 ? stage.first : stage.second;

    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(2);
    tft.setCursor(8, 6);
    tft.print("CHOOSE OPPONENT");
    tft.drawFastHLine(8, 28, 304, ILI9341_CYAN);

    const Enemy* enemies[] = {&stage.first, &stage.second};
    for (int i = 0; i < 2; ++i) {
      const int y = 38 + i * 38;
      if (i == selected) {
        tft.fillRoundRect(8, y, 304, 32, 4, ILI9341_CYAN);
        tft.setTextColor(ILI9341_BLACK);
      } else tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(2);
      tft.setCursor(15, y + 8);
      tft.print(enemies[i]->name);
      tft.setTextSize(1);
      tft.print(" HP:"); tft.print(enemies[i]->life);
      tft.print(" R:"); tft.print(enemies[i]->reward);

    tft.drawFastHLine(8, 118, 304, ILI9341_DARKGREY);
    printWrapped(shown.description, 10, 126, 300, 1, ILI9341_YELLOW, 8);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 226);
    tft.print("Joystick: Choose   A: Fight");
    }

    while (true) {
      Direction d = readDirection();
      if (d == UP || d == DOWN) {
        selected = 1 - selected;
        soundMove();
        waitForRelease();
        break;
      }
      if (confirmDown()) {
        soundSelect();
        waitForRelease();
        return selected;
      }
      delay(10);
    }
  }
}

Enemy chooseEnemy() {
  const StageData& stage = STAGES[currentStage];
  if (stage.boss) {
    messagePage("FINAL OPPONENT",
                String(stage.first.name) + "\n\n" + stage.first.description +
                "\n\nYou were never intended to succeed.",
                ILI9341_RED);
    return stage.first;
  }

  const int choice = chooseOpponentScreen(stage);
  Enemy selected = choice == 0 ? stage.first : stage.second;
  if (choice == 1) ++harderEnemyChoices;
  return selected;
}

// ---------------- Combat ----------------
void combatPage(const Enemy& enemy, const char* title, const String& text,
                uint16_t color) {
  drawBattleStatus(enemy);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(8, 65);
  tft.println(title);
  printWrapped(text, 8, 92, 304, 2, color, 6);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(8, 226);
  tft.print("A or joystick button: Continue");
  waitForRelease();
  while (!confirmDown()) delay(10);
  soundSelect();
  waitForRelease();
}

int chooseCriticalPrediction(const Enemy& enemy, int choices) {
  int selected = 0;
  waitForRelease();
  while (true) {
    drawBattleStatus(enemy);
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(2);
    tft.setCursor(8, 62);
    tft.print("PREDICT 1-"); tft.print(choices);
    for (int i = 0; i < choices; ++i) {
      const int x = 12 + i * 60;
      if (i == selected) {
        tft.fillRoundRect(x, 100, 48, 42, 5, ILI9341_CYAN);
        tft.setTextColor(ILI9341_BLACK);
      } else {
        tft.drawRoundRect(x, 100, 48, 42, 5, ILI9341_WHITE);
        tft.setTextColor(ILI9341_WHITE);
      }
      tft.setTextSize(3);
      tft.setCursor(x + 15, 110);
      tft.print(i + 1);
    }
    printWrapped("Match the hidden number for triple damage. A miss costs 4 HP.",
                 8, 170, 304, 1, ILI9341_YELLOW, 4);

    while (true) {
      Direction d = readDirection();
      if (d == LEFT || d == RIGHT || d == UP || d == DOWN) {
        selected += (d == LEFT || d == UP) ? -1 : 1;
        if (selected < 0) selected = choices - 1;
        if (selected >= choices) selected = 0;
        soundMove(); waitForRelease(); break;
      }
      if (confirmDown()) { soundSelect(); waitForRelease(); return selected + 1; }
      delay(10);
    }
  }
}

int criticalRoll(Enemy& enemy) {
  const int choices = player.diceBonus >= 7 ? 3 : (player.diceBonus >= 3 ? 4 : 5);
  const int prediction = chooseCriticalPrediction(enemy, choices);
  playSfxBlocking(SFX_CRITICAL_ROLL);
  delay(1000);  // Music continues underneath to build suspense.
  const int result = random(1, choices + 1);
  const int roll = diceRoll();
  String text = "Predicted: " + String(prediction) + "\nResult: " + String(result) +
                "\nRoll: " + String(roll);
  if (prediction == result) {
    text += "\n\nCRITICAL HIT!";
    playSfxBlocking(SFX_CRITICAL_WIN);
    combatPage(enemy, "CRITICAL", text, ILI9341_GREEN);
    return (roll + player.luck) * 3;
  }
  player.damage(4);
  playSfxBlocking(SFX_EXPLOSION);
  text += "\n\nFailed: -4 HP";
  combatPage(enemy, "CRITICAL", text, ILI9341_RED);
  return roll + player.luck;
}

int chooseCombatAction(const Enemy& enemy) {
  const char* names[] = {"Normal roll", "Weighted roll", "Critical roll", "Double roll"};
  const char* descriptions[] = {
    "Roll d20, then add Luck and your permanent dice bonus.",
    "Costs 5 coins. Adds damage and restores some of your HP.",
    "Predict correctly for triple damage. Failure costs 4 HP.",
    "Uses one token, rolls twice, and permanently raises Luck."
  };
  int selected = 0;
  waitForRelease();
  while (true) {
    drawBattleStatus(enemy);
    for (int i = 0; i < 4; ++i) {
      const int y = 62 + i * 31;
      if (i == selected) {
        tft.fillRoundRect(8, y, 304, 27, 4, ILI9341_CYAN);
        tft.setTextColor(ILI9341_BLACK);
      } else tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(2);
      tft.setCursor(15, y + 6);
      tft.print(names[i]);
      if (i == 1) { tft.setTextSize(1); tft.print(" [5 coins]"); }
      if (i == 3) { tft.setTextSize(1); tft.print(" ["); tft.print(player.tokens); tft.print("]"); }
    }
    printWrapped(descriptions[selected], 8, 190, 304, 1, ILI9341_YELLOW, 3);

    while (true) {
      Direction d = readDirection();
      if (d == UP || d == DOWN) {
        selected += d == UP ? -1 : 1;
        if (selected < 0) selected = 3;
        if (selected > 3) selected = 0;
        soundMove(); waitForRelease(); break;
      }
      if (confirmDown()) { soundSelect(); waitForRelease(); return selected; }
      delay(10);
    }
  }
}

int playerAttack(Enemy& enemy) {
  while (true) {
    const int choice = chooseCombatAction(enemy);

    if (choice == 0) {
      attackHitCount = 1;
      const int roll = diceRoll();
      combatPage(enemy, "NORMAL ROLL", "Roll: " + String(roll), ILI9341_WHITE);
      return roll + player.luck;
    }
    if (choice == 1) {
      if (player.coins < 5) {
        combatPage(enemy, "NOT AVAILABLE", "You need 5 coins.", ILI9341_RED);
        continue;
      }
      player.coins -= 5;
      const int bonus = player.diceBonus >= 7 ? 8 : (player.diceBonus >= 3 ? 5 : 3);
      const int healing = player.diceBonus >= 7 ? 5 : (player.diceBonus >= 3 ? 4 : 3);
      const int roll = diceRoll();
      player.heal(healing);
      playSfxBlocking(SFX_SPEND);
      attackHitCount = 1;
      combatPage(enemy, "WEIGHTED ROLL",
                  "Roll: " + String(roll) + "\nDamage bonus: +" + String(bonus) +
                  "\nHealed: " + String(healing), ILI9341_GREEN);
      return roll + player.luck + bonus;
    }
    if (choice == 2) {
      attackHitCount = 1;
      return criticalRoll(enemy);
    }
    if (player.tokens <= 0) {
      combatPage(enemy, "NOT AVAILABLE", "No Double Roll token.", ILI9341_RED);
      continue;
    }
    --player.tokens;
    const int first = diceRoll();
    const int second = diceRoll();
    ++player.luck;
    attackHitCount = 2;
    combatPage(enemy, "DOUBLE ROLL",
                "First: " + String(first) + "\nSecond: " + String(second) +
                "\nLuck increased to " + String(player.luck), ILI9341_GREEN);
    return first + second + player.luck;
  }
}

bool battle(Enemy enemy) {
  startMusic(currentStage == STAGE_COUNT - 1 ? MUSIC_BOSS : MUSIC_COMBAT);
  messagePage("BATTLE", String("You face ") + enemy.name + ".", ILI9341_RED);
  while (player.alive() && enemy.life > 0) {
    const int damage = playerAttack(enemy);
    enemy.life = max(0, enemy.life - damage);
    for (int hitNumber = 0; hitNumber < attackHitCount; ++hitNumber) {
      playSfxBlocking(SFX_HIT);
      if (hitNumber + 1 < attackHitCount) delay(100);
    }
    combatPage(enemy, "YOUR ATTACK",
                "Damage: " + String(damage) + "\nEnemy HP: " + String(enemy.life),
                ILI9341_GREEN);
    if (enemy.life <= 0) break;

    const int roll = baseRoll();
    const int enemyDamage = max(1, (roll + 3) / 4 + enemy.luck / 2 + enemy.difficulty / 2);
    player.damage(enemyDamage);
    playSfxBlocking(SFX_HURT);
    combatPage(enemy, "ENEMY ATTACK",
                String(enemy.name) + " rolled " + roll +
                "\nDamage: " + enemyDamage + "\nYour HP: " + player.life,
                COLOR_ORANGE);
  }

  if (!player.alive()) {
    stopMusic();
    delay(30);
    imageMessage(IMAGE_GAME_OVER, "The House always collects.", ILI9341_RED);
    deleteSave();
    return false;
  }

  player.coins += enemy.reward;
  playSfxBlocking(SFX_PICKUP);
  messagePage("VICTORY",
              "Reward: " + String(enemy.reward) + " coins\nTotal: " + player.coins,
              ILI9341_GREEN);
  return true;
}

// ---------------- Store ----------------
void storeMessage(const String& text, bool success = false) {
  messagePage("CASINO STORE", text, success ? ILI9341_GREEN : ILI9341_RED);
}

int chooseStoreItem() {
  const char* names[] = {"Silver Dice +3  40", "Gold Dice +7    80",
                         "Double token    15", "Life Potion      5",
                         "Armor +20 HP   100", "Leave store"};
  const char* descriptions[] = {
    "Permanent +3 to every roll. Also improves Critical and Weighted rolls.",
    "Permanent +7 to every roll. Requires the Silver Dice upgrade.",
    "Adds one Double Roll use. Carry 2 normally, or 3 while wearing armor.",
    "Restores 5 HP, but cannot increase HP beyond your current maximum.",
    "Adds 20 maximum HP and raises the Double Roll token limit to three.",
    "Leave the store and continue toward the next arena."
  };
  int selected = 0;
  waitForRelease();

  while (true) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(2);
    tft.setCursor(8, 5);
    tft.print("STORE");
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(1);
    tft.setCursor(91, 8);
    tft.print("COINS "); tft.print(player.coins);
    tft.print("   HP "); tft.print(player.life); tft.print('/'); tft.print(player.maxLife);
    tft.drawFastHLine(8, 27, 304, ILI9341_CYAN);
    tft.drawFastVLine(199, 32, 142, ILI9341_DARKGREY);

    for (int i = 0; i < 6; ++i) {
      const int y = 34 + i * 24;
      if (i == selected) {
        tft.fillRoundRect(8, y, 185, 20, 3, ILI9341_CYAN);
        tft.setTextColor(ILI9341_BLACK);
      } else tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(1);
      tft.setCursor(14, y + 6);
      tft.print(names[i]);
    }

    // The right panel shows only the currently selected item. Scaling each
    // source pixel to a 4 x 4 block preserves the intentional pixel art.
    tft.drawRoundRect(207, 39, 106, 106, 5, ILI9341_CYAN);
    if (selected < 5) drawStoreIconScaled(selected, 220, 52, 4);

    tft.drawFastHLine(8, 180, 304, ILI9341_DARKGREY);
    printWrapped(descriptions[selected], 8, 187, 304, 1, ILI9341_YELLOW, 4);

    while (true) {
      Direction d = readDirection();
      if (d == UP || d == DOWN) {
        selected += d == UP ? -1 : 1;
        if (selected < 0) selected = 5;
        if (selected > 5) selected = 0;
        soundMove(); waitForRelease(); break;
      }
      if (confirmDown()) { soundSelect(); waitForRelease(); return selected; }
      delay(10);
    }
  }
}

void openStore() {
  startMusic(sdFileExists(MUSIC_STORE) ? MUSIC_STORE : MUSIC_TENSION);
  loadStoreIcons();
  while (true) {
    const int choice = chooseStoreItem();

    if (choice == 5) return;
    if (choice == 0) {
      if (player.diceBonus >= 3) storeMessage("Silver Dice already owned.");
      else if (player.coins < 40) storeMessage("Not enough coins.");
      else { player.coins -= 40; player.diceBonus = 3; soundPurchase(); storeMessage("Silver Dice purchased!", true); }
    } else if (choice == 1) {
      if (player.diceBonus == 7) storeMessage("Gold Dice already owned.");
      else if (player.diceBonus < 3) storeMessage("Buy Silver Dice first.");
      else if (player.coins < 80) storeMessage("Not enough coins.");
      else { player.coins -= 80; player.diceBonus = 7; soundPurchase(); storeMessage("Gold Dice purchased!", true); }
    } else if (choice == 2) {
      if (player.tokens >= player.tokenLimit()) storeMessage("Token carrying limit reached.");
      else if (player.coins < 15) storeMessage("Not enough coins.");
      else { player.coins -= 15; ++player.tokens; soundPurchase(); storeMessage("Double Roll token purchased!", true); }
    } else if (choice == 3) {
      if (player.life >= player.maxLife) storeMessage("Life is already full.");
      else if (player.coins < 5) storeMessage("Not enough coins.");
      else {
        player.coins -= 5;
        player.heal(5);
        playSfxBlocking(SFX_SPEND);
        storeMessage("Recovered 5 HP.", true);
      }
    } else if (choice == 4) {
      if (player.armor) storeMessage("Armor already equipped.");
      else if (player.coins < 100) storeMessage("Not enough coins.");
      else {
        player.coins -= 100; player.armor = true;
        player.maxLife += 20; player.life += 20;
        soundPurchase();
        storeMessage("Armor equipped: +20 HP!", true);
      }
    }
  }
}

// ---------------- Full game ----------------
void playGame(bool continuing) {
  if (!continuing) {
    resetGame();
    showIntro();
  } else if (storePending) {
    openStore();
    storePending = false;
    saveGame();
  }

  while (currentStage < STAGE_COUNT) {
    showStage();
    Enemy enemy = chooseEnemy();
    if (!battle(enemy)) return;

    if (currentStage == STAGE_COUNT - 1) {
      const int score = player.coins + STAGE_COUNT * 10 + harderEnemyChoices * 10;
      deleteSave();
      stopMusic();
      delay(30);
      playSfxBlocking(SFX_PICKUP);
      imageMessage(IMAGE_VICTORY,
                   "The doors unlock. Freedom earned. Score: " + String(score),
                   ILI9341_GREEN);
      return;
    }

    player.heal(5);
    ++currentStage;
    storePending = true;
    saveGame();  // Continue resumes at the next stage.
    soundHeal();
    messagePage("RECOVERY", "You recover 5 HP.\nProgress saved.");
    const char* checkpointItems[] = {"Continue playing", "Main menu"};
    if (chooseMenu("KEEP PLAYING?", checkpointItems, 2) == 1) return;
    openStore();
    storePending = false;
    saveGame();  // Also save purchases.
  }
}

void chooseVolumeLevel() {
  const int levels[] = {25, 50, 75, 100};
  int selected = 3;
  for (int i = 0; i < 4; ++i)
    if (masterVolume == levels[i]) selected = i;

  waitForRelease();
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("VOLUME");
  tft.drawFastHLine(10, 30, 300, ILI9341_CYAN);
  printWrapped("Move left or right, then press A to confirm.",
               10, 45, 300, 1, ILI9341_YELLOW, 3);

  while (true) {
    // Only this small strip is redrawn when the selection moves.
    tft.fillRect(0, 82, 320, 82, ILI9341_BLACK);
    for (int i = 0; i < 4; ++i) {
      const int x = 7 + i * 78;
      if (i == selected) {
        tft.fillRoundRect(x, 96, 70, 44, 5, ILI9341_CYAN);
        tft.setTextColor(ILI9341_BLACK);
      } else {
        tft.drawRoundRect(x, 96, 70, 44, 5, ILI9341_CYAN);
        tft.setTextColor(ILI9341_WHITE);
      }
      tft.setTextSize(2);
      tft.setCursor(x + (levels[i] == 100 ? 10 : 16), 111);
      tft.print(levels[i]);
      tft.print('%');
    }
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(1);
    tft.setCursor(10, 226);
    tft.print("Left/Right: Adjust   A: Select   B: Cancel");

    while (true) {
      const Direction direction = readDirection();
      if (direction == LEFT || direction == RIGHT) {
        selected += direction == LEFT ? -1 : 1;
        if (selected < 0) selected = 3;
        if (selected > 3) selected = 0;
        waitForRelease();
        break;
      }
      if (confirmDown()) {
        masterVolume = levels[selected];
        waitForRelease();
        return;
      }
      if (digitalRead(BUTTON_B) == LOW) {
        waitForRelease();
        return;
      }
      delay(10);
    }
  }
}

void settings() {
  while (true) {
    char volumeLabel[20];
    snprintf(volumeLabel, sizeof(volumeLabel), "Volume: %d%%", masterVolume);
    const char* items[] = {
      musicEnabled ? "Music: ON" : "Music: OFF",
      sfxEnabled ? "Sound effects: ON" : "Sound effects: OFF",
      volumeLabel,
      "Return"
    };
    const int choice = chooseMenu("SETTINGS", items, 4);
    if (choice == 3) return;
    if (!audioReady) messagePage("AUDIO", "Audio is unavailable.", ILI9341_RED);
    else if (choice == 0) {
      musicEnabled = !musicEnabled;
      messagePage("MUSIC", musicEnabled ? "Music ON" : "Music OFF");
    } else if (choice == 1) {
      sfxEnabled = !sfxEnabled;
      if (sfxEnabled) soundSelect();
      messagePage("SOUND EFFECTS", sfxEnabled ? "Sound effects ON" :
                  "Sound effects OFF");
    } else chooseVolumeLevel();
  }
}

void drawMainMenuOverlay(bool canContinue) {
  const char* labels[] = {"NEW GAME", canContinue ? "CONTINUE" : "CONTINUE - NO SAVE", "SETTINGS"};
  // Load the title only once when the menu opens. The cursor is updated
  // separately so joystick movement never rereads the full BMP from SD.
  if (!drawTitleImage()) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(2);
    tft.setCursor(45, 65);
    tft.print("ESCAPE THE CASINO");
  }
  for (int y = 160; y < 240; y += 2)
    tft.drawFastHLine(0, y, 320, ILI9341_BLACK);

  for (int i = 0; i < 3; ++i) {
    const int y = 169 + i * 21;
    tft.setTextColor(i == 1 && !canContinue ? ILI9341_DARKGREY : ILI9341_YELLOW);
    tft.setTextSize(1);
    int16_t x1, y1;
    uint16_t w, h;
    tft.getTextBounds(labels[i], 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((320 - w) / 2, y + 5);
    tft.print(labels[i]);
  }
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 231);
  tft.print("Joystick: Move   A: Select");
}

void drawMainMenuCursor(int selected, bool canContinue) {
  const char* labels[] = {"NEW GAME", canContinue ? "CONTINUE" :
                         "CONTINUE - NO SAVE", "SETTINGS"};
  // Erasing and redrawing these three tiny cursor cells is effectively
  // instant and avoids touching either the SD card or the title artwork.
  for (int i = 0; i < 3; ++i) {
    int16_t x1, y1;
    uint16_t w, h;
    tft.setTextSize(1);
    tft.getTextBounds(labels[i], 0, 0, &x1, &y1, &w, &h);
    const int cursorX = (320 - static_cast<int>(w)) / 2 - 9;
    tft.fillRect(cursorX, 172 + i * 21, 7, 10, ILI9341_BLACK);
  }

  tft.setTextColor(ILI9341_RED);
  tft.setTextSize(1);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(labels[selected], 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((320 - static_cast<int>(w)) / 2 - 8,
                173 + selected * 21);
  tft.print('>');
}

int chooseMainMenu(bool canContinue) {
  int selected = 0;
  waitForRelease();
  drawMainMenuOverlay(canContinue);
  drawMainMenuCursor(selected, canContinue);
  while (true) {
    Direction direction = readDirection();
    if (direction == UP || direction == DOWN) {
      selected += direction == UP ? -1 : 1;
      if (selected < 0) selected = 2;
      if (selected > 2) selected = 0;
      drawMainMenuCursor(selected, canContinue);
      waitForRelease();
    }
    if (confirmDown()) {
      waitForRelease();
      return selected;
    }
    delay(10);
  }
}

void mainMenu() {
  while (true) {
    // startMusic ignores a request for the track already playing, so opening
    // Settings and returning here does not restart the menu soundtrack.
    startMusic(MUSIC_MENU);
    const bool canContinue = sdFileExists(SAVE_PATH);
    const int choice = chooseMainMenu(canContinue);
    if (choice == 0) playGame(false);
    else if (choice == 1) {
      if (!canContinue || !loadGame())
        messagePage("CONTINUE", "No valid save was found.", ILI9341_RED);
      else
        playGame(true);
    } else settings();
  }
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);
  sdMutex = xSemaphoreCreateMutex();
  pinMode(BUTTON_X, INPUT_PULLUP);
  pinMode(BUTTON_Y, INPUT_PULLUP);
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(JOYSTICK_SW, INPUT_PULLUP);
  pinMode(TFT_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  SPI.begin(TFT_SCK, SD_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(20, 85);
  tft.println("Starting console...");

  sdReady = SD.begin(SD_CS, SPI, 4000000);
  Serial.println(sdReady ? "SD ready" : "SD unavailable");
  if (sdReady) {
    Serial.println(cacheTitleImage() ? "Title cached in PSRAM" :
                                      "Title cache unavailable");
  }

  audio.setPins(I2S_BCLK, I2S_LRC, I2S_DIN);
  audioReady = audio.begin(I2S_MODE_STD, SAMPLE_RATE,
                           I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!audioReady) {
    musicEnabled = false;
    sfxEnabled = false;
  }
  Serial.println(audioReady ? "Audio ready" : "Audio unavailable");

  if (audioReady) {
    xTaskCreatePinnedToCore(audioTask, "casinoAudio", 8192, nullptr, 2, nullptr, 0);
  }

  randomSeed(analogRead(JOYSTICK_X) ^ micros());
  delay(250);
}

void loop() {
  mainMenu();
}
