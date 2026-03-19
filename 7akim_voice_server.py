#!/usr/bin/env python3
"""
7akim Voice Backend Server v2.0
Receives WAV audio from ESP32 → Whisper STT → LLM (with HA function calling) → ElevenLabs TTS → returns PCM audio

Environment Variables:
  OPENAI_API_KEY       — OpenAI API key (for Whisper + LLM)
  ELEVENLABS_KEY       — ElevenLabs API key (for TTS)
  ELEVENLABS_VOICE     — ElevenLabs voice ID (default: Sarah)
  HA_URL               — Home Assistant URL (e.g., http://192.168.68.79:8123 or https://ha.alibondabo.com)
  HA_TOKEN             — Home Assistant long-lived access token
  LLM_MODEL            — LLM model to use (default: gpt-4o-mini)
  SERVER_PORT          — Server port (default: 5050)
"""

import os
import io
import json
import struct
import tempfile
import time
import logging
import requests
from flask import Flask, request, Response, jsonify
from openai import OpenAI

# ─── Logging ─────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format='[7akim] %(asctime)s %(levelname)s: %(message)s',
    datefmt='%H:%M:%S'
)
log = logging.getLogger('7akim')

app = Flask(__name__)

# ─── Configuration ───────────────────────────────────
OPENAI_API_KEY   = os.environ.get("OPENAI_API_KEY", "")
ELEVENLABS_KEY   = os.environ.get("ELEVENLABS_KEY", "")
ELEVENLABS_VOICE = os.environ.get("ELEVENLABS_VOICE", "EXAVITQu4vr4xnSDxMaL")  # Sarah
HA_URL           = os.environ.get("HA_URL", "http://192.168.68.79:8123")
HA_TOKEN         = os.environ.get("HA_TOKEN", "")
LLM_MODEL        = os.environ.get("LLM_MODEL", "gpt-4o-mini")
SERVER_PORT      = int(os.environ.get("SERVER_PORT", "5050"))

openai_client = OpenAI(api_key=OPENAI_API_KEY) if OPENAI_API_KEY else None

# ─── Home Assistant Client ────────────────────────────
class HomeAssistantClient:
    """Handles all communication with Home Assistant REST API."""
    
    def __init__(self, url, token):
        self.url = url.rstrip('/')
        self.token = token
        self.headers = {
            'Authorization': f'Bearer {token}',
            'Content-Type': 'application/json',
        }
        self.entity_cache = {}
        self.cache_time = 0
        self.CACHE_TTL = 30  # seconds
    
    def _get(self, endpoint):
        try:
            resp = requests.get(f'{self.url}/api/{endpoint}', headers=self.headers, timeout=10)
            resp.raise_for_status()
            return resp.json()
        except Exception as e:
            log.error(f"HA GET {endpoint} failed: {e}")
            return None
    
    def _post(self, endpoint, data=None):
        try:
            resp = requests.post(f'{self.url}/api/{endpoint}', headers=self.headers, json=data or {}, timeout=10)
            resp.raise_for_status()
            return resp.json() if resp.content else {"success": True}
        except Exception as e:
            log.error(f"HA POST {endpoint} failed: {e}")
            return None
    
    def is_connected(self):
        result = self._get('')
        return result is not None
    
    def get_states(self):
        """Get all entity states, with caching."""
        now = time.time()
        if now - self.cache_time < self.CACHE_TTL and self.entity_cache:
            return self.entity_cache
        states = self._get('states')
        if states:
            self.entity_cache = {s['entity_id']: s for s in states}
            self.cache_time = now
        return self.entity_cache
    
    def get_entity_state(self, entity_id):
        states = self.get_states()
        return states.get(entity_id)
    
    def call_service(self, domain, service, entity_id=None, data=None):
        """Call a Home Assistant service."""
        payload = data or {}
        if entity_id:
            payload['entity_id'] = entity_id
        result = self._post(f'services/{domain}/{service}', payload)
        log.info(f"HA service: {domain}.{service} entity={entity_id} data={data} result={'OK' if result else 'FAIL'}")
        return result
    
    def get_weather(self):
        state = self.get_entity_state('weather.forecast_home')
        if state:
            attrs = state.get('attributes', {})
            return {
                'condition': state.get('state', 'unknown'),
                'temperature': attrs.get('temperature'),
                'humidity': attrs.get('humidity'),
                'wind_speed': attrs.get('wind_speed'),
            }
        return None

ha = HomeAssistantClient(HA_URL, HA_TOKEN)

# ─── Room → Entity Mapping ───────────────────────────
ROOM_ENTITIES = {
    'office': {
        'light': 'light.office_smart_led_bulb',
        'climate': 'climate.office',
    },
    'living room': {
        'light': 'light.led_bulb_living_room_2',
        'climate': 'climate.living_room',
    },
    'bedroom': {
        'climate': 'climate.bedroom_thermostat',
    },
    'kids room': {
        'light': 'light.salma_floor_lamp_basic_2',
        'climate': 'climate.talya_salma_ali',
    },
    'garage': {
        'light': 'light.guarage_light',
        'camera': 'camera.guarage_live_view',
    },
    'front': {
        'light': 'light.front_light',
        'camera': 'camera.front_live_view',
    },
    'backyard': {
        'light': 'light.backyard_light',
        'camera': 'camera.backyard_live_view',
    },
}

# Arabic room name aliases
ROOM_ALIASES = {
    'المكتب': 'office',
    'مكتب': 'office',
    'الصالة': 'living room',
    'صالة': 'living room',
    'غرفة المعيشة': 'living room',
    'غرفة النوم': 'bedroom',
    'غرفة الأطفال': 'kids room',
    'غرفة سلمى': 'kids room',
    'الجراج': 'garage',
    'كراج': 'garage',
    'الأمام': 'front',
    'الخلف': 'backyard',
}

# ─── LLM Function Definitions (OpenAI tools format) ──
HA_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "control_light",
            "description": "Turn a light on or off, or set brightness/color. Rooms: office, living room, kids room, garage, front, backyard, net lights, curtain lights.",
            "parameters": {
                "type": "object",
                "properties": {
                    "room": {"type": "string", "description": "Room name (e.g., 'office', 'living room', 'kids room', 'garage')"},
                    "action": {"type": "string", "enum": ["on", "off", "toggle"], "description": "Light action"},
                    "brightness": {"type": "integer", "description": "Brightness 0-255 (optional)"},
                },
                "required": ["room", "action"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "control_climate",
            "description": "Set AC temperature or turn AC on/off. Rooms: office, living room, bedroom, kids room.",
            "parameters": {
                "type": "object",
                "properties": {
                    "room": {"type": "string", "description": "Room name"},
                    "temperature": {"type": "number", "description": "Target temperature in Celsius (optional)"},
                    "mode": {"type": "string", "enum": ["cool", "off"], "description": "HVAC mode (optional)"},
                },
                "required": ["room"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "control_media",
            "description": "Control media players (Samsung Odyssey Ark TV, LG OLED TV). Actions: turn_on, turn_off, volume_up, volume_down, pause, play.",
            "parameters": {
                "type": "object",
                "properties": {
                    "device": {"type": "string", "description": "Device name: 'samsung tv' or 'lg tv'"},
                    "action": {"type": "string", "enum": ["turn_on", "turn_off", "volume_up", "volume_down", "pause", "play"]},
                },
                "required": ["device", "action"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "get_sensor_data",
            "description": "Get current sensor readings: temperature, humidity, weather, camera battery levels, network speed.",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "What to check: 'temperature in office', 'weather', 'all temperatures', 'camera batteries'"},
                },
                "required": ["query"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "control_switch",
            "description": "Toggle switches: net lights, curtain lights, camera motion detection.",
            "parameters": {
                "type": "object",
                "properties": {
                    "entity_id": {"type": "string", "description": "Full entity_id of the switch"},
                    "action": {"type": "string", "enum": ["on", "off", "toggle"]},
                },
                "required": ["entity_id", "action"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "scene_command",
            "description": "Execute predefined scenes: 'all off' (turn everything off), 'movie mode' (dim lights, TV on), 'goodnight' (all lights off, ACs to sleep temp).",
            "parameters": {
                "type": "object",
                "properties": {
                    "scene": {"type": "string", "description": "Scene name: 'all off', 'movie mode', 'goodnight', 'good morning'"},
                },
                "required": ["scene"]
            }
        }
    },
]

# ─── HA Function Execution ────────────────────────────
def resolve_room(room_name):
    """Resolve room name (including Arabic) to canonical name."""
    room = room_name.lower().strip()
    if room in ROOM_ALIASES:
        room = ROOM_ALIASES[room]
    return room

def execute_ha_function(func_name, args):
    """Execute an HA function and return result string."""
    try:
        if func_name == "control_light":
            room = resolve_room(args.get('room', ''))
            action = args.get('action', 'toggle')
            brightness = args.get('brightness')
            
            # Special lights (not room-based)
            special_lights = {
                'net lights': 'light.net_lights',
                'curtain lights': 'light.curtain_lights',
            }
            
            if room in special_lights:
                entity_id = special_lights[room]
            elif room in ROOM_ENTITIES and 'light' in ROOM_ENTITIES[room]:
                entity_id = ROOM_ENTITIES[room]['light']
            else:
                return f"No light found for room '{room}'"
            
            data = {}
            if brightness is not None:
                data['brightness'] = brightness
            
            service = 'turn_on' if action == 'on' else ('turn_off' if action == 'off' else 'toggle')
            result = ha.call_service('light', service, entity_id, data if data else None)
            return f"Light {action} in {room}" if result else f"Failed to control light in {room}"
        
        elif func_name == "control_climate":
            room = resolve_room(args.get('room', ''))
            temp = args.get('temperature')
            mode = args.get('mode')
            
            if room not in ROOM_ENTITIES or 'climate' not in ROOM_ENTITIES[room]:
                return f"No AC found for room '{room}'"
            
            entity_id = ROOM_ENTITIES[room]['climate']
            
            if mode:
                ha.call_service('climate', 'set_hvac_mode', entity_id, {'hvac_mode': mode})
            if temp:
                ha.call_service('climate', 'set_temperature', entity_id, {'temperature': temp})
            
            parts = []
            if mode: parts.append(f"mode={mode}")
            if temp: parts.append(f"temp={temp}°C")
            return f"AC in {room}: {', '.join(parts)}" if parts else f"AC in {room} unchanged"
        
        elif func_name == "control_media":
            device = args.get('device', '').lower()
            action = args.get('action', '')
            
            if 'samsung' in device or 'odyssey' in device or 'ark' in device:
                entity_id = 'media_player.yehia_55_odyssey_ark_g9_ls55bg970nmxue'
            elif 'lg' in device or 'oled' in device:
                entity_id = 'media_player.lg_webos_tv_oled65c9pva'
            else:
                return f"Unknown media device: {device}"
            
            service_map = {
                'turn_on': 'turn_on', 'turn_off': 'turn_off',
                'volume_up': 'volume_up', 'volume_down': 'volume_down',
                'pause': 'media_pause', 'play': 'media_play',
            }
            service = service_map.get(action, action)
            result = ha.call_service('media_player', service, entity_id)
            return f"TV {action}" if result else f"Failed to control TV"
        
        elif func_name == "get_sensor_data":
            query = args.get('query', '').lower()
            states = ha.get_states()
            
            if 'weather' in query:
                weather = ha.get_weather()
                if weather:
                    return f"Weather: {weather['condition']}, {weather['temperature']}°C, humidity {weather['humidity']}%"
                return "Weather data unavailable"
            
            if 'temperature' in query or 'temp' in query or 'حرارة' in query:
                temps = []
                for room, entities in ROOM_ENTITIES.items():
                    if 'climate' in entities:
                        state = states.get(entities['climate'])
                        if state:
                            current = state.get('attributes', {}).get('current_temperature', '?')
                            temps.append(f"{room}: {current}°C")
                if 'all' in query or not any(r in query for r in ROOM_ENTITIES):
                    return "Temperatures: " + ", ".join(temps)
                for room in ROOM_ENTITIES:
                    if room in query:
                        state = states.get(ROOM_ENTITIES[room].get('climate', ''))
                        if state:
                            current = state.get('attributes', {}).get('current_temperature', '?')
                            return f"{room} temperature: {current}°C"
                return "Temperatures: " + ", ".join(temps)
            
            if 'battery' in query or 'camera' in query:
                batteries = []
                for eid, state in states.items():
                    if 'battery' in eid and 'ring' in eid.lower():
                        batteries.append(f"{state.get('attributes', {}).get('friendly_name', eid)}: {state.get('state', '?')}%")
                return "Camera batteries: " + ", ".join(batteries) if batteries else "No battery data"
            
            return "I can check: temperatures, weather, camera batteries"
        
        elif func_name == "control_switch":
            entity_id = args.get('entity_id', '')
            action = args.get('action', 'toggle')
            service = 'turn_on' if action == 'on' else ('turn_off' if action == 'off' else 'toggle')
            result = ha.call_service('switch', service, entity_id)
            return f"Switch {action}: {entity_id}" if result else f"Failed to control switch"
        
        elif func_name == "scene_command":
            scene = args.get('scene', '').lower()
            
            if scene == 'all off':
                for room, entities in ROOM_ENTITIES.items():
                    if 'light' in entities:
                        ha.call_service('light', 'turn_off', entities['light'])
                ha.call_service('light', 'turn_off', 'light.net_lights')
                ha.call_service('light', 'turn_off', 'light.curtain_lights')
                return "All lights turned off"
            
            elif scene == 'goodnight':
                for room, entities in ROOM_ENTITIES.items():
                    if 'light' in entities:
                        ha.call_service('light', 'turn_off', entities['light'])
                ha.call_service('light', 'turn_off', 'light.net_lights')
                ha.call_service('light', 'turn_off', 'light.curtain_lights')
                ha.call_service('climate', 'set_temperature', 'climate.bedroom_thermostat', {'temperature': 23})
                return "Goodnight! All lights off, bedroom AC set to 23°C"
            
            elif scene == 'movie mode':
                ha.call_service('light', 'turn_off', 'light.led_bulb_living_room_2')
                ha.call_service('media_player', 'turn_on', 'media_player.lg_webos_tv_oled65c9pva')
                return "Movie mode: living room lights off, TV on"
            
            elif scene == 'good morning':
                ha.call_service('light', 'turn_on', 'light.office_smart_led_bulb', {'brightness': 255})
                ha.call_service('light', 'turn_on', 'light.led_bulb_living_room_2', {'brightness': 200})
                return "Good morning! Office and living room lights on"
            
            return f"Unknown scene: {scene}"
        
        return f"Unknown function: {func_name}"
    
    except Exception as e:
        log.error(f"HA function error: {func_name} {args} -> {e}")
        return f"Error executing {func_name}: {str(e)}"

# ─── Conversation ────────────────────────────────────
SYSTEM_PROMPT = """You are 7akim (حكيم), a smart AI home assistant running on an ESP32 device in Yehia's home in Dubai.

You can control the smart home through Home Assistant. Available rooms and devices:
- Office: light, AC (Nest thermostat)
- Living Room: light, AC (Nest thermostat)  
- Bedroom: AC (Nest thermostat)
- Kids Room (Talya/Salma/Ali): light (Salma's floor lamp), AC (Nest thermostat)
- Garage, Front, Backyard: lights (Ring cameras)
- Net Lights, Curtain Lights: decorative lights
- Samsung Odyssey Ark TV, LG OLED C9 TV
- Ring cameras: garage, front, backyard

IMPORTANT RULES:
1. Keep responses SHORT — 1-3 sentences max, they will be spoken aloud
2. Always reply in the SAME LANGUAGE the user spoke (Arabic → Arabic, English → English)
3. When the user asks to control something, USE THE TOOLS — don't just say you will
4. Be friendly, natural, and concise — you're Jarvis, not a chatbot
5. For temperatures, the AC units support 'cool' and 'off' modes only (Dubai climate)
6. If you're unsure about an entity, ask for clarification
7. You can chain multiple actions (e.g., "turn off all lights" = multiple light.turn_off calls)

Scenes you can execute:
- "all off" — turn off all lights
- "goodnight" — all lights off, bedroom AC to 23°C
- "movie mode" — living room lights off, TV on
- "good morning" — office and living room lights on bright
"""

conversation = [{"role": "system", "content": SYSTEM_PROMPT}]


# ─── Audio Processing ────────────────────────────────
def wav_to_pcm(wav_bytes):
    idx = wav_bytes.find(b'data')
    if idx == -1:
        return wav_bytes
    data_size = struct.unpack_from('<I', wav_bytes, idx + 4)[0]
    return wav_bytes[idx + 8: idx + 8 + data_size]


def pcm_to_wav(pcm_bytes, sample_rate=16000, channels=1, bit_depth=16):
    buf = io.BytesIO()
    byte_rate = sample_rate * channels * (bit_depth // 8)
    block_align = channels * (bit_depth // 8)
    data_len = len(pcm_bytes)
    buf.write(b'RIFF')
    buf.write(struct.pack('<I', 36 + data_len))
    buf.write(b'WAVE')
    buf.write(b'fmt ')
    buf.write(struct.pack('<IHHIIHH', 16, 1, channels, sample_rate,
                          byte_rate, block_align, bit_depth))
    buf.write(b'data')
    buf.write(struct.pack('<I', data_len))
    buf.write(pcm_bytes)
    return buf.getvalue()


def transcribe_audio(wav_bytes):
    with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as f:
        f.write(wav_bytes)
        tmp_path = f.name
    try:
        with open(tmp_path, 'rb') as f:
            result = openai_client.audio.transcriptions.create(
                model="whisper-1",
                file=f
            )
        return result.text.strip()
    finally:
        os.unlink(tmp_path)


def ask_llm(user_text):
    """Send text to LLM with HA function calling support."""
    global conversation
    
    conversation.append({"role": "user", "content": user_text})
    
    ha_action_summary = ""
    
    # First call — may return tool calls
    resp = openai_client.chat.completions.create(
        model=LLM_MODEL,
        messages=conversation,
        tools=HA_TOOLS if HA_TOKEN else None,
        max_tokens=300
    )
    
    msg = resp.choices[0].message
    
    # Handle tool calls (function calling)
    if msg.tool_calls:
        conversation.append(msg)  # Add assistant message with tool calls
        
        for tool_call in msg.tool_calls:
            func_name = tool_call.function.name
            try:
                func_args = json.loads(tool_call.function.arguments)
            except json.JSONDecodeError:
                func_args = {}
            
            log.info(f"LLM tool call: {func_name}({func_args})")
            result = execute_ha_function(func_name, func_args)
            ha_action_summary = result
            log.info(f"HA result: {result}")
            
            conversation.append({
                "role": "tool",
                "tool_call_id": tool_call.id,
                "content": result
            })
        
        # Second call — LLM generates natural language response based on tool results
        resp2 = openai_client.chat.completions.create(
            model=LLM_MODEL,
            messages=conversation,
            max_tokens=200
        )
        reply = resp2.choices[0].message.content.strip()
        conversation.append({"role": "assistant", "content": reply})
    else:
        reply = msg.content.strip() if msg.content else "I'm not sure what to do."
        conversation.append({"role": "assistant", "content": reply})
    
    # Keep conversation manageable
    if len(conversation) > 30:
        # Keep system prompt + last 20 messages
        conversation = [conversation[0]] + conversation[-20:]
    
    return reply, ha_action_summary


def text_to_speech(text):
    url = f"https://api.elevenlabs.io/v1/text-to-speech/{ELEVENLABS_VOICE}"
    headers = {
        "xi-api-key": ELEVENLABS_KEY,
        "Content-Type": "application/json"
    }
    payload = {
        "text": text,
        "model_id": "eleven_turbo_v2",
        "voice_settings": {
            "stability": 0.5,
            "similarity_boost": 0.75
        }
    }
    resp = requests.post(url, headers=headers, json=payload, timeout=30)
    if resp.status_code == 200:
        return resp.content
    log.error(f"TTS failed: {resp.status_code} {resp.text[:200]}")
    return None


def mp3_to_pcm(mp3_bytes, target_rate=16000):
    with tempfile.NamedTemporaryFile(suffix='.mp3', delete=False) as f:
        f.write(mp3_bytes)
        mp3_path = f.name
    pcm_path = mp3_path.replace('.mp3', '.pcm')
    try:
        os.system(
            f"ffmpeg -y -i {mp3_path} -f s16le -ar {target_rate} -ac 1 {pcm_path} -loglevel quiet"
        )
        if os.path.exists(pcm_path):
            with open(pcm_path, 'rb') as f:
                return f.read()
    finally:
        os.unlink(mp3_path)
        if os.path.exists(pcm_path):
            os.unlink(pcm_path)
    return None


# ─── Routes ──────────────────────────────────────────
@app.route('/voice', methods=['POST'])
def voice_endpoint():
    start_time = time.time()
    log.info(f"Received {len(request.data)} bytes audio")
    
    wav_bytes = request.data
    if not wav_bytes:
        return "No audio", 400

    # Step 1: Transcribe
    try:
        transcript = transcribe_audio(wav_bytes)
        log.info(f"Heard: '{transcript}'")
    except Exception as e:
        log.error(f"STT error: {e}")
        return "STT failed", 500

    if not transcript:
        return "Nothing heard", 204

    # Step 2: LLM with HA function calling
    try:
        reply, ha_action = ask_llm(transcript)
        log.info(f"Reply: '{reply}'")
        if ha_action:
            log.info(f"HA Action: '{ha_action}'")
    except Exception as e:
        log.error(f"LLM error: {e}")
        return "LLM failed", 500

    # Step 3: TTS
    try:
        mp3_bytes = text_to_speech(reply)
        if not mp3_bytes:
            raise Exception("Empty TTS response")
    except Exception as e:
        log.error(f"TTS error: {e}")
        return "TTS failed", 500

    # Step 4: Convert to PCM for ESP32
    try:
        pcm = mp3_to_pcm(mp3_bytes, target_rate=16000)
        if not pcm:
            raise Exception("PCM conversion failed")
    except Exception as e:
        log.error(f"PCM conversion error: {e}")
        return "Audio conversion failed", 500

    elapsed = time.time() - start_time
    log.info(f"Total pipeline: {elapsed:.1f}s | PCM: {len(pcm)} bytes")

    # Return PCM with metadata headers
    response = Response(pcm, mimetype='application/octet-stream')
    response.headers['X-7akim-Text'] = reply[:200]  # Truncate for header safety
    response.headers['X-7akim-HA-Action'] = ha_action[:200] if ha_action else ''
    response.headers['X-7akim-Transcript'] = transcript[:200]
    response.headers['X-7akim-Time'] = f"{elapsed:.1f}s"
    return response


@app.route('/health', methods=['GET'])
def health():
    ha_status = "connected" if (HA_TOKEN and ha.is_connected()) else "disconnected"
    return jsonify({
        "status": "ok",
        "name": "7akim-voice-server",
        "version": "2.0",
        "ha_status": ha_status,
        "ha_url": HA_URL,
        "llm_model": LLM_MODEL,
        "tts": "elevenlabs",
    })


@app.route('/ha/states', methods=['GET'])
def ha_states():
    """Debug endpoint: get all HA states."""
    if not HA_TOKEN:
        return jsonify({"error": "HA not configured"}), 503
    states = ha.get_states()
    return jsonify({"count": len(states), "entities": list(states.keys())})


@app.route('/ha/test', methods=['POST'])
def ha_test():
    """Debug endpoint: test an HA service call."""
    data = request.json or {}
    domain = data.get('domain', 'light')
    service = data.get('service', 'toggle')
    entity_id = data.get('entity_id', '')
    result = ha.call_service(domain, service, entity_id, data.get('data'))
    return jsonify({"success": result is not None, "result": str(result)})


@app.route('/conversation/reset', methods=['POST'])
def reset_conversation():
    """Reset conversation history."""
    global conversation
    conversation = [{"role": "system", "content": SYSTEM_PROMPT}]
    return jsonify({"status": "reset", "messages": 1})


# ─── Main ────────────────────────────────────────────
if __name__ == '__main__':
    print("=" * 55)
    print("  7akim Voice Backend Server v2.0")
    print("  Home Assistant Integration Enabled")
    print(f"  HA URL: {HA_URL}")
    print(f"  HA Token: {'SET' if HA_TOKEN else 'NOT SET'}")
    print(f"  LLM Model: {LLM_MODEL}")
    print(f"  TTS: ElevenLabs (voice: {ELEVENLABS_VOICE})")
    print(f"  Listening on 0.0.0.0:{SERVER_PORT}")
    print("=" * 55)
    
    # Test HA connection on startup
    if HA_TOKEN:
        if ha.is_connected():
            states = ha.get_states()
            log.info(f"HA connected! {len(states)} entities loaded")
        else:
            log.warning(f"HA at {HA_URL} not reachable — will retry on requests")
    else:
        log.warning("HA_TOKEN not set — Home Assistant integration disabled")
    
    app.run(host='0.0.0.0', port=SERVER_PORT, debug=False)
