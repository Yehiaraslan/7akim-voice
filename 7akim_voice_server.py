#!/usr/bin/env python3
"""
7akim Voice Backend Server
Receives WAV audio from ESP32 → Whisper STT → Claude → ElevenLabs TTS → returns PCM audio
"""

import os
import io
import struct
import tempfile
import requests
from flask import Flask, request, Response
from openai import OpenAI

app = Flask(__name__)

# ─── API Keys ─────────────────────────────────────────
OPENAI_API_KEY   = "YOUR_OPENAI_API_KEY"
ELEVENLABS_KEY   = "YOUR_ELEVENLABS_API_KEY"
ELEVENLABS_VOICE = "EXAVITQu4vr4xnSDxMaL"  # "Sarah" - natural voice, change as needed

# ─── Conversation history (simple, in-memory) ─────────
conversation = [
    {
        "role": "system",
        "content": (
            "You are 7akim, a helpful AI assistant running on an ESP32 knob device. "
            "Keep responses SHORT — 1-3 sentences max, since they will be spoken aloud. "
            "IMPORTANT: Always reply in the SAME language the user spoke in. "
            "If they speak Arabic, reply in Arabic. If English, reply in English. "
            "You can control a Home Assistant smart home at 192.168.68.79:8123. "
            "Be friendly, natural, and concise."
        )
    }
]

openai_client = OpenAI(api_key=OPENAI_API_KEY)


def wav_to_pcm(wav_bytes):
    """Strip WAV header, return raw PCM bytes."""
    # Find 'data' chunk
    idx = wav_bytes.find(b'data')
    if idx == -1:
        return wav_bytes
    data_size = struct.unpack_from('<I', wav_bytes, idx + 4)[0]
    return wav_bytes[idx + 8: idx + 8 + data_size]


def pcm_to_wav(pcm_bytes, sample_rate=16000, channels=1, bit_depth=16):
    """Wrap raw PCM in WAV header."""
    buf = io.BytesIO()
    byte_rate = sample_rate * channels * (bit_depth // 8)
    block_align = channels * (bit_depth // 8)
    data_len = len(pcm_bytes)
    # RIFF header
    buf.write(b'RIFF')
    buf.write(struct.pack('<I', 36 + data_len))
    buf.write(b'WAVE')
    # fmt chunk
    buf.write(b'fmt ')
    buf.write(struct.pack('<IHHIIHH', 16, 1, channels, sample_rate,
                          byte_rate, block_align, bit_depth))
    # data chunk
    buf.write(b'data')
    buf.write(struct.pack('<I', data_len))
    buf.write(pcm_bytes)
    return buf.getvalue()


def transcribe_audio(wav_bytes):
    """Send WAV to Whisper, return transcript text."""
    with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as f:
        f.write(wav_bytes)
        tmp_path = f.name
    try:
        with open(tmp_path, 'rb') as f:
            result = openai_client.audio.transcriptions.create(
                model="whisper-1",
                file=f
                # no language= so Whisper auto-detects Arabic, English, etc.
            )
        return result.text.strip()
    finally:
        os.unlink(tmp_path)


def ask_claude(user_text):
    """Send text to GPT-4o-mini via OpenAI, return response."""
    conversation.append({"role": "user", "content": user_text})
    
    resp = openai_client.chat.completions.create(
        model="gpt-4o-mini",
        messages=conversation,
        max_tokens=200
    )
    reply = resp.choices[0].message.content.strip()
    
    conversation.append({"role": "assistant", "content": reply})
    # Keep conversation history manageable (last 20 messages)
    if len(conversation) > 21:
        conversation[1:3] = []  # Remove oldest user/assistant pair, keep system prompt
    
    return reply


def text_to_speech(text):
    """Send text to ElevenLabs, return MP3 bytes."""
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
        return resp.content  # MP3 bytes
    return None


def mp3_to_pcm(mp3_bytes, target_rate=16000):
    """Convert MP3 to raw PCM using ffmpeg."""
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


# ─── Route ────────────────────────────────────────────
@app.route('/voice', methods=['POST'])
def voice_endpoint():
    print(f"\n[7akim] Received {len(request.data)} bytes audio")
    
    wav_bytes = request.data
    if not wav_bytes:
        return "No audio", 400

    # Step 1: Transcribe
    try:
        transcript = transcribe_audio(wav_bytes)
        print(f"[7akim] Heard: '{transcript}'")
    except Exception as e:
        print(f"[7akim] STT error: {e}")
        return "STT failed", 500

    if not transcript:
        return "Nothing heard", 204

    # Step 2: Ask Claude
    try:
        reply = ask_claude(transcript)
        print(f"[7akim] Reply: '{reply}'")
    except Exception as e:
        print(f"[7akim] LLM error: {e}")
        return "LLM failed", 500

    # Step 3: TTS
    try:
        mp3_bytes = text_to_speech(reply)
        if not mp3_bytes:
            raise Exception("Empty TTS response")
    except Exception as e:
        print(f"[7akim] TTS error: {e}")
        return "TTS failed", 500

    # Step 4: Convert to PCM for ESP32
    try:
        pcm = mp3_to_pcm(mp3_bytes, target_rate=16000)
        if not pcm:
            raise Exception("PCM conversion failed")
        print(f"[7akim] Sending {len(pcm)} bytes PCM back to device")
    except Exception as e:
        print(f"[7akim] PCM conversion error: {e}")
        return "Audio conversion failed", 500

    return Response(pcm, mimetype='application/octet-stream')


@app.route('/health', methods=['GET'])
def health():
    return {"status": "ok", "name": "7akim-voice-server"}, 200


if __name__ == '__main__':
    print("=" * 50)
    print("  7akim Voice Backend Server")
    print("  Listening on 0.0.0.0:5050")
    print("=" * 50)
    app.run(host='0.0.0.0', port=5050, debug=False)
