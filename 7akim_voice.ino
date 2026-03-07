/*
 * 7akim Voice Assistant - ESP32-S3 Knob Display
 * Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 * fqbn: esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=huge_app,USBMode=default,CPUFreq=240
 *
 * Navigation: Touch screen button (hold to record, release to send)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "lcd_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "cst816.h"
#include "lcd_config.h"
#include "user_config.h"

// ─── State ───────────────────────────────────────────
typedef enum {
  STATE_IDLE, STATE_RECORDING, STATE_PROCESSING, STATE_PLAYING, STATE_ERROR
} device_state_t;
volatile device_state_t device_state = STATE_IDLE;

// ─── Touch trigger ────────────────────────────────────
static EventGroupHandle_t touch_events = NULL;
#define TOUCH_START_BIT  BIT0
#define TOUCH_STOP_BIT   BIT1

// ─── Audio ───────────────────────────────────────────
static int16_t          *rec_buf    = NULL;
static size_t            rec_bytes  = 0;
static i2s_chan_handle_t  tx_chan    = NULL;
static i2s_chan_handle_t  rx_chan    = NULL;

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

// ─── Touch polling task ───────────────────────────────
// Polls CST816 directly — bypasses LVGL event system for reliability
static void touch_poll_task(void *arg) {
  uint16_t tx, ty;
  bool was_touched = false;
  for (;;) {
    bool touched = (getTouch(&tx, &ty) != 0);
    if (touched && !was_touched) {
      // Finger down
      if (device_state == STATE_IDLE) {
        xEventGroupSetBits(touch_events, TOUCH_START_BIT);
      }
    } else if (!touched && was_touched) {
      // Finger up
      if (device_state == STATE_RECORDING) {
        xEventGroupSetBits(touch_events, TOUCH_STOP_BIT);
      }
    }
    was_touched = touched;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// LVGL button callback (kept for visual feedback only, events handled by touch_poll_task)
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

  // Touch button
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
  lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Hold to Talk");
  lv_obj_set_style_text_font(ui_btn_label, &lv_font_montserrat_16, 0);
  lv_obj_center(ui_btn_label);

  lvgl_release();
}

// ─── Audio hardware ───────────────────────────────────
void init_audio() {
  gpio_config_t gc = {};
  gc.pin_bit_mask = (1ULL << PCM_ENABLE_PIN);
  gc.mode = GPIO_MODE_OUTPUT;
  gc.pull_up_en = GPIO_PULLUP_ENABLE;
  gc.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&gc);
  gpio_set_level((gpio_num_t)PCM_ENABLE_PIN, 1);

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

  i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&rx_cfg, NULL, &rx_chan);
  i2s_pdm_rx_config_t pdm_rx = {
    .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = (gpio_num_t)PDM_CLK_PIN, .din = (gpio_num_t)PDM_DATA_PIN,
      .invert_flags = {false},
    },
  };
  i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx);
  i2s_channel_enable(rx_chan);
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

// ─── Send audio → get TTS ─────────────────────────────
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
      play_audio(resp, got);
      heap_caps_free(resp);
    }
  }
  http.end();
  return true;
}

// ─── Voice task ───────────────────────────────────────
void voice_task(void *arg) {
  size_t bytes_read = 0;
  for (;;) {
    switch (device_state) {
      case STATE_IDLE:
        xEventGroupWaitBits(touch_events, TOUCH_START_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        rec_bytes = 0;
        device_state = STATE_RECORDING;
        ui_set("7akim", "Listening...", 0xc0392b);
        lvgl_acquire();
        if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_STOP "  Release to Send");
        lvgl_release();
        break;

      case STATE_RECORDING:
        if (rec_bytes + 1024 <= AUDIO_BUF_SIZE) {
          i2s_channel_read(rx_chan, (char*)rec_buf+rec_bytes, 1024, &bytes_read, 100);
          rec_bytes += bytes_read;
        }
        if (xEventGroupGetBits(touch_events) & TOUCH_STOP_BIT) {
          xEventGroupClearBits(touch_events, TOUCH_STOP_BIT);
          device_state = STATE_PROCESSING;
          ui_set("7akim", "Thinking...", 0xe67e22);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Hold to Talk");
          lvgl_release();
        }
        if (rec_bytes >= AUDIO_BUF_SIZE) {
          device_state = STATE_PROCESSING;
          ui_set("7akim", "Thinking...", 0xe67e22);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Hold to Talk");
          lvgl_release();
        }
        break;

      case STATE_PROCESSING:
        if (send_and_play()) {
          device_state = STATE_IDLE;
          ui_set("7akim", "Tap to talk", 0x1a1a2e);
        } else {
          device_state = STATE_ERROR;
          ui_set("7akim", "Error!", 0x7f8c8d);
          vTaskDelay(pdMS_TO_TICKS(2000));
          device_state = STATE_IDLE;
          ui_set("7akim", "Tap to talk", 0x1a1a2e);
        }
        break;

      default:
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─── setup ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  // Wait for LVGL task to fully start
  vTaskDelay(pdMS_TO_TICKS(300));

  build_ui();  // builds UI under mutex

  ui_set("7akim", "Connecting WiFi", 0x2980b9);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    ui_set("7akim", WiFi.localIP().toString().c_str(), 0x27ae60);
    vTaskDelay(pdMS_TO_TICKS(1500));
  } else {
    ui_set("7akim", "No WiFi!", 0xc0392b);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  rec_buf = (int16_t*)heap_caps_malloc(AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!rec_buf) {
    ui_set("7akim", "PSRAM failed!", 0xc0392b);
    while(1) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  init_audio();
  touch_events = xEventGroupCreate();

  device_state = STATE_IDLE;
  ui_set("7akim", "Tap to talk", 0x1a1a2e);

  xTaskCreatePinnedToCore(voice_task,      "voice",  8192, NULL, 5, NULL, 1);
  xTaskCreate(touch_poll_task, "touch_poll", 2048, NULL, 4, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
