
#include <Arduino.h>
#include <LittleFS.h>
#include <driver/i2s.h>

#define I2S_PORT I2S_NUM_0

// MAX98357A pins (same as your original)
#define I2S_LRC  14
#define I2S_BCLK 25
#define I2S_DOUT 32
#define AMP_GAIN 27

// WAV files must be 22050 Hz, 16-bit, mono PCM
const int SAMPLE_RATE = 22050;
const float VOLUME = 0.20f;

// Original 8 button pins
const int BUTTON_PINS[8] = {
  13, 4, 18, 19, 22, 23, 26, 33
};

// File played by each button
const char* SOUND_FILES[8] = {
  "/sound1.wav",
  "/sound2.wav",
  "/sound3.wav",
  "/sound4.wav",
  "/sound5.wav",
  "/sound6.wav",
  "/sound7.wav",
  "/sound8.wav"
};

File audioFile;
int previousButton = -1;
uint32_t audioBytesLeft = 0;

// Read little-endian WAV values
uint16_t read16(File &f) {
  uint16_t a = f.read();
  uint16_t b = f.read();
  return a | (b << 8);
}

uint32_t read32(File &f) {
  uint32_t a = read16(f);
  uint32_t b = read16(f);
  return a | (b << 16);
}

// Find and validate WAV audio data
bool openWav(const char* path) {
  audioFile = LittleFS.open(path, "r");

  if (!audioFile) {
    Serial.printf("Missing file: %s\n", path);
    return false;
  }

  char id[4];

  if (audioFile.readBytes(id, 4) != 4 ||
      memcmp(id, "RIFF", 4) != 0) {
    audioFile.close();
    return false;
  }

  read32(audioFile); // RIFF size

  if (audioFile.readBytes(id, 4) != 4 ||
      memcmp(id, "WAVE", 4) != 0) {
    audioFile.close();
    return false;
  }

  bool formatOK = false;

  // WAV files contain chunks such as fmt and data
  while (audioFile.available() >= 8) {
    if (audioFile.readBytes(id, 4) != 4) break;
    uint32_t size = read32(audioFile);
    uint32_t start = audioFile.position();
    uint32_t next = start + size + (size & 1);

    if (next > audioFile.size()) break;

    if (memcmp(id, "fmt ", 4) == 0 && size >= 16) {
      uint16_t format = read16(audioFile);
      uint16_t channels = read16(audioFile);
      uint32_t rate = read32(audioFile);

      read32(audioFile); // Byte rate
      read16(audioFile); // Block alignment
      uint16_t bits = read16(audioFile);

      formatOK = (format == 1 &&
                  channels == 1 &&
                  rate == SAMPLE_RATE &&
                  bits == 16);
    }

    if (memcmp(id, "data", 4) == 0) {
      if (!formatOK) break;
      audioBytesLeft = size;
      Serial.printf("Playing: %s\n", path);
      return true;
    }

    audioFile.seek(next);
  }

  Serial.printf("Unsupported WAV: %s\n", path);
  audioFile.close();
  return false;
}

void stopSound() {
  if (audioFile) audioFile.close();
  audioBytesLeft = 0;
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 8; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
  }

  pinMode(AMP_GAIN, OUTPUT);
  digitalWrite(AMP_GAIN, LOW);

  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed!");
  } else {
    Serial.println("LittleFS ready!");
  }

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(
      I2S_MODE_MASTER | I2S_MODE_TX
    ),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);

  Serial.println("Custom Soundboard Ready!");
}

void loop() {
  int pressed = -1;

  // Read buttons
  for (int i = 0; i < 8; i++) {
    if (digitalRead(BUTTON_PINS[i]) == LOW) {
      pressed = i;
      break;
    }
  }

  // Start sound only on a new button press
  if (pressed != -1 && pressed != previousButton) {
    stopSound();
    openWav(SOUND_FILES[pressed]);
  }

  previousButton = pressed;

  const int FRAMES = 64;
  int16_t mono[FRAMES];
  int16_t stereo[FRAMES * 2] = {0};

  if (audioFile && audioBytesLeft > 0) {
    size_t toRead = min(
      (size_t)audioBytesLeft,
      sizeof(mono)
    );

    // Always read complete 16-bit samples
    toRead &= ~((size_t)1);

    size_t count = audioFile.read(
      (uint8_t*)mono, toRead
    );

    audioBytesLeft -= count;

    // Convert mono to stereo for MAX98357A
    for (int i = 0; i < count / 2; i++) {
      int16_t sample = (int16_t)(
        mono[i] * VOLUME
      );

      stereo[i * 2] = sample;
      stereo[i * 2 + 1] = sample;
    }

    if (audioBytesLeft == 0 || count == 0) {
      stopSound();
    }
  }

  size_t bytesWritten;
  i2s_write(
    I2S_PORT,
    stereo,
    sizeof(stereo),
    &bytesWritten,
    portMAX_DELAY
  );
}
