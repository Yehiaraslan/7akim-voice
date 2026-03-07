# 7akim Voice Assistant - Setup & Reference

## Hardware
- **Device:** Waveshare ESP32-S3-Knob-Touch-LCD-1.8
- **Display:** 1.8" round AMOLED, 360×360, SH8601 driver
- **Touch:** CST816 capacitive touch (I2C)
- **Audio out:** PCM5100A DAC → 3.5mm headphone jack
- **Mic:** Built-in digital PDM microphone
- **Knob:** Rotary encoder (rotate only, no press on ESP32-S3 side)
- **fqbn:** `esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=huge_app,USBMode=default,CPUFreq=240`

## Network Architecture
```
ESP32 (192.168.68.x via YehiaRaslan_Downstairs WiFi)
    ↓ HTTP POST /voice
Windows PC (192.168.68.73)
    ↓ netsh portproxy → port 5050
Ubuntu VM via Hyper-V (172.31.102.164)
    ↓ Flask server
Voice Pipeline: Whisper STT → GPT-4o-mini → ElevenLabs TTS
```

## WiFi Config
- **ESP connects to:** `YehiaRaslan_Downstairs` (gets 192.168.68.x IP)
- **Server IP in ESP:** `192.168.68.73` (Windows PC)
- **Server port:** `5050`
- Other WiFi: `Yehiaraslan` → 192.168.1.x subnet (DIFFERENT network, can't reach server)

## Windows Port Forwarding (required on every reboot)
Run in **Admin CMD** on Windows:
```
netsh interface portproxy delete v4tov4 listenport=5050 listenaddress=0.0.0.0
netsh interface portproxy add v4tov4 listenport=5050 listenaddress=0.0.0.0 connectport=5050 connectaddress=172.31.102.164
netsh advfirewall firewall add rule name="7akim-voice" dir=in action=allow protocol=TCP localport=5050
```
Check it's set: `netsh interface portproxy show all`

## USB Passthrough (Hyper-V → Ubuntu)
When ESP USB needs to be attached to Ubuntu VM (for flashing):
1. On Windows, install usbipd: `winget install usbipd`
2. Share the device: `usbipd bind --busid <BUSID>` (ESP32 is usually busid 5-4)
3. From **Ubuntu**: `sudo usbip attach -r 172.31.96.1 -b 5-4`
4. Fix permissions: `sudo chmod 666 /dev/ttyACM0`
5. Flash normally with arduino-cli

Check BUSID on Windows: `usbipd list`

## Flashing (from Ubuntu)
```bash
# Compile
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=huge_app,USBMode=default,CPUFreq=240" /home/yehia/sketches/7akim_voice/

# Upload
sudo chmod 666 /dev/ttyACM0
arduino-cli upload -p /dev/ttyACM0 --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=huge_app,USBMode=default,CPUFreq=240" /home/yehia/sketches/7akim_voice/
```

## Starting the Voice Server (Ubuntu)
```bash
nohup python3 /home/yehia/7akim_voice_server.py > /home/yehia/7akim_server.log 2>&1 &
tail -f /home/yehia/7akim_server.log
```
Check if running: `pgrep -f 7akim_voice_server`

## How to Use the Device
1. Device boots → connects to WiFi → shows IP briefly → "Tap to talk"
2. **Tap and hold** anywhere on screen → circle turns RED → recording
3. **Release** → circle turns ORANGE → sending + thinking
4. Circle turns GREEN → speaking response through headphone jack
5. Returns to "Tap to talk"

## Key Fixes Applied
- **White screen:** LVGL runs in its own FreeRTOS task with a mutex. All UI calls must use `lvgl_acquire()` / `lvgl_release()` wrappers. Added public wrappers in `lcd_bsp.c`.
- **Touch not working:** LVGL button events unreliable. Added dedicated `touch_poll_task` that directly polls CST816 chip every 20ms.
- **Knob:** Device has rotary encoder but it cannot be pressed — rotate only. Replaced knob interaction with touch screen button.

## Voice Pipeline (server)
- **STT:** OpenAI Whisper (auto language detection — supports Arabic + English)
- **LLM:** GPT-4o-mini (replies in same language as user)
- **TTS:** ElevenLabs (voice: Sarah, model: eleven_turbo_v2)
- **Audio format:** ESP sends WAV → server returns raw PCM 16kHz 16-bit mono

## Files
- `7akim_voice.ino` — Main sketch
- `user_config.h` — WiFi, server IP, pin config
- `lcd_bsp.c/h` — Display + LVGL init (modified: added lvgl_acquire/release)
- `cst816.cpp/h` — Touch driver
- `7akim_voice_server.py` — Python Flask voice backend
