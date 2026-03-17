/*
 * 7akim Voice Assistant - ESP32-S3 Knob Display
 * Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 * fqbn: esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240
 *
 * Wake word: "Jarvis" (on-device WakeNet9, ~300ms latency)
 * Fallback: Touch screen button (hold to record, release to send)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "lcd_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "cst816.h"
#include "lcd_config.h"
#include "user_config.h"

// ─── ESP_SR (WakeNet) ────────────────────────────────
#include <ESP_I2S.h>
#include <ESP_SR.h>

// ESP_SR command list (required even if we only use wake word mode)
static const sr_cmd_t sr_commands[] = {
  {0, "Turn on the light"},
  {1, "Turn off the light"},
};

// I2SClass instance for PDM microphone (shared with ESP_SR)
I2SClass i2s_mic;

// ─── State ───────────────────────────────────────────
typedef enum {
  STATE_LISTENING,    // WakeNet is listening for "Jarvis"
  STATE_RECORDING,    // Recording user's voice command
  STATE_PROCESSING,   // Sending to server, waiting for response
  STATE_PLAYING,      // Playing back TTS audio
  STATE_ERROR         // Error state (temporary)
} device_state_t;
volatile device_state_t device_state = STATE_LISTENING;

// ─── Event flags ─────────────────────────────────────
static EventGroupHandle_t app_events = NULL;
#define EVT_WAKEWORD_BIT   BIT0    // WakeNet detected "Jarvis"
#define EVT_TOUCH_START_BIT BIT1   // Touch started (fallback)
#define EVT_TOUCH_STOP_BIT  BIT2   // Touch released
#define EVT_REC_TIMEOUT_BIT BIT3   // Recording timeout (auto-stop after silence)

// ─── Audio ───────────────────────────────────────────
static int16_t          *rec_buf    = NULL;
static size_t            rec_bytes  = 0;
static i2s_chan_handle_t  tx_chan    = NULL;

// ─── UI handles ───────────────────────────────────────
static lv_obj_t *ui_circle     = NULL;
static lv_obj_t *ui_main       = NULL;
static lv_obj_t *ui_sub        = NULL;
static lv_obj_t *ui_btn        = NULL;
static lv_obj_t *ui_btn_label  = NULL;

// ─── Thread-safe UI update ────────────────────────────
void ui_set(const char *main_txt, const char *sub_txt, uint32_t color) {
  lvgl_acquire();
  if (ui_circle) lv_obj_set_style_bg_color(ui_circle, lv_color_hex(color), 0);
  if (ui_main && main_txt) lv_label_set_text(ui_main, main_txt);
  if (ui_sub  && sub_txt)  lv_label_set_text(ui_sub,  sub_txt);
  lvgl_release();
}

// ─── ESP_SR wake word callback ────────────────────────
// Called by ESP_SR from its internal task when wake word or command is detected
void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  switch (event) {
    case SR_EVENT_WAKEWORD:
      Serial.println("[7akim] Wake word 'Jarvis' detected!");
      if (device_state == STATE_LISTENING) {
        xEventGroupSetBits(app_events, EVT_WAKEWORD_BIT);
      }
      break;
    case SR_EVENT_WAKEWORD_CHANNEL:
      Serial.printf("[7akim] Wake word channel verified (phrase_id=%d)\n", phrase_id);
      break;
    case SR_EVENT_COMMAND:
      Serial.printf("[7akim] Command detected: id=%d\n", command_id);
      break;
    case SR_EVENT_TIMEOUT:
      Serial.println("[7akim] SR timeout");
      break;
    default:
      break;
  }
}

// ─── Touch polling task ───────────────────────────────
// Polls CST816 directly — bypasses LVGL event system for reliability
static void touch_poll_task(void *arg) {
  uint16_t tx, ty;
  bool was_touched = false;
  for (;;) {
    bool touched = (getTouch(&tx, &ty) != 0);
    if (touched && !was_touched) {
      // Finger down — start recording (fallback trigger)
      if (device_state == STATE_LISTENING) {
        xEventGroupSetBits(app_events, EVT_TOUCH_START_BIT);
      }
    } else if (!touched && was_touched) {
      // Finger up — stop recording
      if (device_state == STATE_RECORDING) {
        xEventGroupSetBits(app_events, EVT_TOUCH_STOP_BIT);
      }
    }
    was_touched = touched;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// LVGL button callback (kept for visual feedback only)
static void btn_event_cb(lv_event_t *e) {
  // visual only — touch_poll_task handles the actual logic
}

void build_ui() {
  lvgl_acquire();

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0a0a), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Main status circle
  ui_circle = lv_obj_create(scr);
  lv_obj_set_size(ui_circle, 220, 220);
  lv_obj_align(ui_circle, LV_ALIGN_CENTER, 0, -35);
  lv_obj_set_style_radius(ui_circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui_circle, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_style_border_width(ui_circle, 4, 0);
  lv_obj_set_style_border_color(ui_circle, lv_color_hex(0x3498db), 0);
  lv_obj_set_style_shadow_width(ui_circle, 25, 0);
  lv_obj_set_style_shadow_color(ui_circle, lv_color_hex(0x3498db), 0);
  lv_obj_clear_flag(ui_circle, LV_OBJ_FLAG_SCROLLABLE);

  ui_main = lv_label_create(ui_circle);
  lv_label_set_text(ui_main, "7akim");
  lv_obj_set_style_text_font(ui_main, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(ui_main, lv_color_hex(0xffffff), 0);
  lv_obj_align(ui_main, LV_ALIGN_CENTER, 0, -15);

  ui_sub = lv_label_create(ui_circle);
  lv_label_set_text(ui_sub, "Starting...");
  lv_obj_set_style_text_font(ui_sub, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(ui_sub, lv_color_hex(0xaaaaaa), 0);
  lv_obj_align(ui_sub, LV_ALIGN_CENTER, 0, 20);

  // Touch button (fallback trigger)
  ui_btn = lv_btn_create(scr);
  lv_obj_set_size(ui_btn, 200, 55);
  lv_obj_align(ui_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_radius(ui_btn, 28, 0);
  lv_obj_set_style_bg_color(ui_btn, lv_color_hex(0x3498db), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(ui_btn, lv_color_hex(0xc0392b), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(ui_btn, 15, 0);
  lv_obj_set_style_shadow_color(ui_btn, lv_color_hex(0x3498db), 0);
  lv_obj_add_event_cb(ui_btn, btn_event_cb, LV_EVENT_ALL, NULL);

  ui_btn_label = lv_label_create(ui_btn);
  lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Say 'Jarvis'");
  lv_obj_set_style_text_font(ui_btn_label, &lv_font_montserrat_16, 0);
  lv_obj_center(ui_btn_label);

  lvgl_release();
}

// ─── Speaker hardware (I2S TX via raw ESP-IDF driver) ──
void init_speaker() {
  // Enable PCM5100A DAC
  gpio_config_t gc = {};
  gc.pin_bit_mask = (1ULL << PCM_ENABLE_PIN);
  gc.mode = GPIO_MODE_OUTPUT;
  gc.pull_up_en = GPIO_PULLUP_ENABLE;
  gc.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&gc);
  gpio_set_level((gpio_num_t)PCM_ENABLE_PIN, 1);

  // I2S TX channel for speaker (I2S_NUM_1, separate from mic)
  i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  i2s_new_channel(&tx_cfg, &tx_chan, NULL);
  i2s_std_config_t tx_std = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_BCLK_PIN,
      .ws   = (gpio_num_t)I2S_WS_PIN, .dout = (gpio_num_t)I2S_DOUT_PIN,
      .din  = I2S_GPIO_UNUSED, .invert_flags = {false, false, false},
    },
  };
  i2s_channel_init_std_mode(tx_chan, &tx_std);
  i2s_channel_enable(tx_chan);
}

// ─── PDM Microphone via Arduino I2SClass ─────────────
// Shared between ESP_SR (wake word) and manual recording
void init_microphone() {
  i2s_mic.setTimeout(1000);
  i2s_mic.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);
  i2s_mic.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  Serial.println("[7akim] PDM microphone initialized via I2SClass");
}

// ─── WAV builder ─────────────────────────────────────
void build_wav_header(uint8_t *h, uint32_t pcm_bytes) {
  uint32_t total=pcm_bytes+36, br=SAMPLE_RATE*2;
  uint16_t pt=1, ch=1, ba=2, bd=16; uint32_t sr=SAMPLE_RATE, fs=16;
  memcpy(h,"RIFF",4); memcpy(h+4,&total,4); memcpy(h+8,"WAVE",4);
  memcpy(h+12,"fmt ",4); memcpy(h+16,&fs,4); memcpy(h+20,&pt,2);
  memcpy(h+22,&ch,2); memcpy(h+24,&sr,4); memcpy(h+28,&br,4);
  memcpy(h+32,&ba,2); memcpy(h+34,&bd,2);
  memcpy(h+36,"data",4); memcpy(h+40,&pcm_bytes,4);
}

// ─── Playback ─────────────────────────────────────────
void play_audio(uint8_t *data, size_t len) {
  size_t written=0, offset=0;
  while (offset < len) {
    size_t chunk = min((size_t)2048, len-offset);
    i2s_channel_write(tx_chan, data+offset, chunk, &written, 1000);
    offset += written;
  }
}

// ─── Send audio to server and play response ───────────
bool send_and_play() {
  if (rec_bytes < 2000) return false;

  uint32_t wav_size = rec_bytes + 44;
  uint8_t *wav = (uint8_t*)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
  if (!wav) return false;
  build_wav_header(wav, rec_bytes);
  memcpy(wav+44, rec_buf, rec_bytes);

  HTTPClient http;
  http.begin(String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/voice");
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(30000);
  int code = http.POST(wav, wav_size);
  heap_caps_free(wav);

  if (code != 200) { http.end(); return false; }

  int plen = http.getSize();
  if (plen > 0 && plen < (int)AUDIO_BUF_SIZE) {
    uint8_t *resp = (uint8_t*)heap_caps_malloc(plen, MALLOC_CAP_SPIRAM);
    if (resp) {
      WiFiClient *stream = http.getStreamPtr();
      size_t got = 0;
      unsigned long t = millis();
      while (got < (size_t)plen && millis()-t < 20000) {
        int av = stream->available();
        if (av>0) got += stream->readBytes(resp+got, av);
        else delay(1);
      }
      ui_set("7akim", "Speaking...", 0x27ae60);
      device_state = STATE_PLAYING;
      play_audio(resp, got);
      heap_caps_free(resp);
    }
  }
  http.end();
  return true;
}

// ─── Record audio from PDM mic (while ESP_SR is paused) ─
void record_audio_chunk() {
  if (rec_bytes + 1024 <= AUDIO_BUF_SIZE) {
    size_t got = i2s_mic.readBytes((char*)rec_buf + rec_bytes, 1024);
    rec_bytes += got;
  }
}

// ─── Voice task ───────────────────────────────────────
// Main state machine: handles wake word, recording, processing
void voice_task(void *arg) {
  unsigned long rec_start_time = 0;

  for (;;) {
    switch (device_state) {

      case STATE_LISTENING: {
        // Wait for either wake word or touch
        EventBits_t bits = xEventGroupWaitBits(
          app_events,
          EVT_WAKEWORD_BIT | EVT_TOUCH_START_BIT,
          pdTRUE,   // clear on exit
          pdFALSE,  // any bit
          portMAX_DELAY
        );

        bool from_wakeword = (bits & EVT_WAKEWORD_BIT);
        bool from_touch    = (bits & EVT_TOUCH_START_BIT);

        if (from_wakeword || from_touch) {
          // Pause ESP_SR so we can read from the mic directly
          ESP_SR.pause();
          Serial.println("[7akim] ESP_SR paused, starting recording");

          rec_bytes = 0;
          rec_start_time = millis();
          device_state = STATE_RECORDING;

          if (from_wakeword) {
            ui_set("7akim", "Listening...", 0xc0392b);
            lvgl_acquire();
            if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_STOP "  Speak now...");
            lvgl_release();
          } else {
            ui_set("7akim", "Listening...", 0xc0392b);
            lvgl_acquire();
            if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_STOP "  Release to Send");
            lvgl_release();
          }
        }
        break;
      }

      case STATE_RECORDING: {
        // Read audio from mic
        record_audio_chunk();

        // Check for stop conditions
        bool stop = false;

        // Touch release stops recording
        if (xEventGroupGetBits(app_events) & EVT_TOUCH_STOP_BIT) {
          xEventGroupClearBits(app_events, EVT_TOUCH_STOP_BIT);
          stop = true;
          Serial.println("[7akim] Touch released, stopping recording");
        }

        // Auto-stop after MAX_REC_SECONDS
        if (millis() - rec_start_time >= (unsigned long)(MAX_REC_SECONDS * 1000)) {
          stop = true;
          Serial.println("[7akim] Max recording time reached");
        }

        // Buffer full
        if (rec_bytes >= AUDIO_BUF_SIZE) {
          stop = true;
          Serial.println("[7akim] Recording buffer full");
        }

        // For wake word triggered recording: auto-stop after a silence gap
        // We use a simple timeout: 5 seconds after wake word, auto-stop
        // (The user speaks their command within 5 seconds)
        if (millis() - rec_start_time >= WAKEWORD_REC_TIMEOUT_MS) {
          stop = true;
          Serial.println("[7akim] Wake word recording timeout");
        }

        if (stop) {
          device_state = STATE_PROCESSING;
          ui_set("7akim", "Thinking...", 0xe67e22);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_REFRESH "  Processing...");
          lvgl_release();
          Serial.printf("[7akim] Recorded %d bytes, sending to server\n", rec_bytes);
        }
        break;
      }

      case STATE_PROCESSING: {
        if (send_and_play()) {
          // Success — resume wake word detection
          device_state = STATE_LISTENING;
          ui_set("7akim", "Say 'Jarvis'", 0x1a1a2e);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Say 'Jarvis'");
          lvgl_release();
        } else {
          device_state = STATE_ERROR;
          ui_set("7akim", "Error!", 0x7f8c8d);
          vTaskDelay(pdMS_TO_TICKS(2000));
          device_state = STATE_LISTENING;
          ui_set("7akim", "Say 'Jarvis'", 0x1a1a2e);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Say 'Jarvis'");
          lvgl_release();
        }

        // Resume ESP_SR wake word detection
        ESP_SR.resume();
        Serial.println("[7akim] ESP_SR resumed, listening for wake word");
        break;
      }

      default:
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─── setup ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[7akim] 7akim Voice Assistant starting...");
  Serial.println("[7akim] Wake word: 'Jarvis' (WakeNet9, on-device)");

  // ── Display + Touch ──
  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  vTaskDelay(pdMS_TO_TICKS(300));

  build_ui();

  // ── WiFi ──
  ui_set("7akim", "Connecting WiFi", 0x2980b9);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[7akim] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    ui_set("7akim", WiFi.localIP().toString().c_str(), 0x27ae60);
    vTaskDelay(pdMS_TO_TICKS(1500));
  } else {
    Serial.println("[7akim] WiFi connection failed!");
    ui_set("7akim", "No WiFi!", 0xc0392b);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // ── Audio buffer (PSRAM) ──
  rec_buf = (int16_t*)heap_caps_malloc(AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!rec_buf) {
    Serial.println("[7akim] PSRAM allocation failed!");
    ui_set("7akim", "PSRAM failed!", 0xc0392b);
    while(1) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  Serial.printf("[7akim] Audio buffer: %d bytes in PSRAM\n", AUDIO_BUF_SIZE);

  // ── Speaker (I2S TX on I2S_NUM_1) ──
  init_speaker();
  Serial.println("[7akim] Speaker initialized (I2S_NUM_1)");

  // ── PDM Microphone (Arduino I2SClass on I2S_NUM_0) ──
  init_microphone();

  // ── ESP_SR Wake Word Engine ──
  ui_set("7akim", "Loading WakeNet...", 0x9b59b6);
  Serial.println("[7akim] Initializing ESP_SR with 'Jarvis' wake word...");

  ESP_SR.onEvent(onSrEvent);
  bool sr_ok = ESP_SR.begin(
    i2s_mic,
    sr_commands,
    sizeof(sr_commands) / sizeof(sr_cmd_t),
    SR_CHANNELS_MONO,       // Single PDM mic = mono
    SR_MODE_WAKEWORD,       // Start in wake word detection mode
    "MN"                    // Input format: M=mic, N=unused
  );

  if (sr_ok) {
    Serial.println("[7akim] ESP_SR started successfully! Listening for 'Jarvis'...");
  } else {
    Serial.println("[7akim] ESP_SR failed to start! Falling back to touch-only mode.");
    ui_set("7akim", "WakeNet failed!", 0xe74c3c);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // ── Event group ──
  app_events = xEventGroupCreate();

  // ── Ready ──
  device_state = STATE_LISTENING;
  ui_set("7akim", "Say 'Jarvis'", 0x1a1a2e);
  Serial.println("[7akim] Ready! Say 'Jarvis' or touch the screen to talk.");

  // ── Tasks ──
  xTaskCreatePinnedToCore(voice_task, "voice", 8192, NULL, 5, NULL, 1);
  xTaskCreate(touch_poll_task, "touch_poll", 2048, NULL, 4, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
