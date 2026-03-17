# 7akim Voice Assistant - Setup & Reference

## Hardware
- **Device:** Waveshare ESP32-S3-Knob-Touch-LCD-1.8
- **Display:** 1.8" round AMOLED, 360x360, SH8601 driver
- **Touch:** CST816 capacitive touch (I2C)
- **Audio out:** PCM5100A DAC -> 3.5mm headphone jack
- **Mic:** Built-in digital PDM microphone
- **Knob:** Rotary encoder (rotate only, no press on ESP32-S3 side)
- **fqbn:** `esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240`

## Wake Word: "Jarvis"
The device uses **WakeNet9** (Espressif ESP-SR) for on-device wake word detection.
- **Wake word:** "Jarvis"
- **Latency:** ~200-500ms from utterance to detection
- **Engine:** WakeNet9 running on ESP32-S3 with PSRAM
- **Partition:** `esp_sr_16` (required for model storage: 3MB APP, 7MB SPIFFS, 2.9MB MODEL)
- **Fallback:** Touch screen still works as a backup trigger (hold to record, release to send)

### How It Works
1. ESP_SR continuously listens via the PDM mic for "Jarvis"
2. On detection, ESP_SR is **paused** and the mic switches to manual recording mode
3. The device records your voice command (up to 5 seconds by default, configurable via `WAKEWORD_REC_TIMEOUT_MS`)
4. Audio is sent to the server for processing (Whisper STT -> GPT-4o-mini -> ElevenLabs TTS)
5. Response plays through the headphone jack
6. ESP_SR **resumes** listening for the next "Jarvis"

### Arduino IDE Board Settings
When using Arduino IDE, select:
- **Board:** ESP32S3 Dev Module
- **PSRAM:** OPI PSRAM
- **Flash Mode:** DIO
- **Flash Size:** 16MB
- **Partition Scheme:** `SR 16M (3MB APP/7MB SPIFFS/2.9MB MODEL)`
- **USB Mode:** Default
- **CPU Frequency:** 240MHz

### Required Libraries
- **ESP_SR** (included with esp32 Arduino core 3.x)
- **ESP_I2S** (included with esp32 Arduino core 3.x)

## Network Architecture
```
ESP32 (192.168.68.x via YehiaRaslan_Downstairs WiFi)
    | HTTP POST /voice
Windows PC (192.168.68.73)
    | netsh portproxy -> port 5050
Ubuntu VM via Hyper-V (172.31.102.164)
    | Flask server
Voice Pipeline: Whisper STT -> GPT-4o-mini -> ElevenLabs TTS
```

## WiFi Config
- **ESP connects to:** `YehiaRaslan_Downstairs` (gets 192.168.68.x IP)
- **Server IP in ESP:** `192.168.68.73` (Windows PC)
- **Server port:** `5050`
- Other WiFi: `Yehiaraslan` -> 192.168.1.x subnet (DIFFERENT network, can't reach server)

## Windows Port Forwarding (required on every reboot)
Run in **Admin CMD** on Windows:
```
netsh interface portproxy delete v4tov4 listenport=5050 listenaddress=0.0.0.0
netsh interface portproxy add v4tov4 listenport=5050 listenaddress=0.0.0.0 connectport=5050 connectaddress=172.31.102.164
netsh advfirewall firewall add rule name="7akim-voice" dir=in action=allow protocol=TCP localport=5050
```
Check it's set: `netsh interface portproxy show all`

## USB Passthrough (Hyper-V -> Ubuntu)
When ESP USB needs to be attached to Ubuntu VM (for flashing):
1. On Windows, install usbipd: `winget install usbipd`
2. Share the device: `usbipd bind --busid <BUSID>` (ESP32 is usually busid 5-4)
3. From **Ubuntu**: `sudo usbip attach -r 172.31.96.1 -b 5-4`
4. Fix permissions: `sudo chmod 666 /dev/ttyACM0`
5. Flash normally with arduino-cli

Check BUSID on Windows: `usbipd list`

## Flashing (from Ubuntu)
```bash
# Compile (NOTE: partition scheme changed from huge_app to esp_sr_16)
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240" /home/yehia/sketches/7akim_voice/

# Upload
sudo chmod 666 /dev/ttyACM0
arduino-cli upload -p /dev/ttyACM0 --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240" /home/yehia/sketches/7akim_voice/
```

## Starting the Voice Server (Ubuntu)
```bash
nohup python3 /home/yehia/7akim_voice_server.py > /home/yehia/7akim_server.log 2>&1 &
tail -f /home/yehia/7akim_server.log
```
Check if running: `pgrep -f 7akim_voice_server`

## How to Use the Device
1. Device boots -> connects to WiFi -> loads WakeNet model -> shows "Say 'Jarvis'"
2. **Say "Jarvis"** -> circle turns RED -> device is listening for your command
3. Speak your command (up to 5 seconds) -> circle turns ORANGE -> processing
4. Circle turns GREEN -> speaking response through headphone jack
5. Returns to "Say 'Jarvis'" and listens again
6. **Alternative:** Touch and hold the screen button to record manually (release to send)

## Key Fixes Applied
- **White screen:** LVGL runs in its own FreeRTOS task with a mutex. All UI calls must use `lvgl_acquire()` / `lvgl_release()` wrappers. Added public wrappers in `lcd_bsp.c`.
- **Touch not working:** LVGL button events unreliable. Added dedicated `touch_poll_task` that directly polls CST816 chip every 20ms.
- **Knob:** Device has rotary encoder but it cannot be pressed -- rotate only. Replaced knob interaction with touch screen button.
- **Wake word integration:** ESP_SR shares the PDM mic (I2S_NUM_0) via Arduino I2SClass. On wake word detection, ESP_SR is paused, mic is read directly for recording, then ESP_SR is resumed. Speaker uses separate I2S_NUM_1 channel.

## Voice Pipeline (server)
- **STT:** OpenAI Whisper (auto language detection -- supports Arabic + English)
- **LLM:** GPT-4o-mini (replies in same language as user)
- **TTS:** ElevenLabs (voice: Sarah, model: eleven_turbo_v2)
- **Audio format:** ESP sends WAV -> server returns raw PCM 16kHz 16-bit mono

## Files
- `7akim_voice.ino` -- Main sketch (wake word + touch + voice pipeline)
- `user_config.h` -- WiFi, server IP, pin config, wake word settings
- `lcd_bsp.c/h` -- Display + LVGL init (modified: added lvgl_acquire/release)
- `cst816.cpp/h` -- Touch driver
- `7akim_voice_server.py` -- Python Flask voice backend

## Troubleshooting

### ESP_SR fails to start
- Ensure partition scheme is `esp_sr_16` (not `huge_app`)
- The WakeNet model needs the MODEL partition (~2.9MB)
- Check serial output for specific error messages
- If model loading crashes, try erasing flash first: `esptool.py erase_flash`

### Wake word not detecting
- Speak clearly: "Jarvis" (English pronunciation)
- Keep within ~1 meter of the device
- Check serial output for `[7akim] Wake word 'Jarvis' detected!` messages
- Background noise can reduce accuracy -- WakeNet works best in quiet environments

### Recording too short / too long
- Adjust `WAKEWORD_REC_TIMEOUT_MS` in `user_config.h` (default: 5000ms)
- For longer commands, increase to 8000-10000
- For snappier responses, decrease to 3000
