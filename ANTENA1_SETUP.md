# Antena 1 Local Assistant Setup

This branch contains the ESP32-S3 firmware changes and the local voice
assistant server used by the Antena 1 prototype.

## Repository contents

- ESP-IDF firmware for the ESP-VoCat board.
- Local Python voice server in `local_voice_assistant/server.py`.
- Antena 1 display assets and radio stream player.

The following runtime artifacts are intentionally not committed:

- `build/`
- `managed_components/`
- Python virtual environments
- local STT/TTS models under `local_voice_assistant/models/`

## Firmware build

Use ESP-IDF 5.5.2 or newer compatible with the project manifest.

```bash
. /home/paulo/esp/esp-idf-v5.5.2/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash
```

The default config targets the ESP-VoCat board, disables the original Chinese
wake word, uses the custom Antena 1 screen, and points OTA/local services to:

```text
http://192.168.2.160:8088
```

If the server machine uses another IP, update `sdkconfig.defaults` and the
constants in `main/application.cc`.

## Local server

```bash
cd local_voice_assistant
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python3 server.py
```

The server expects:

- Ollama running locally on `127.0.0.1:11434`.
- Model `qwen2.5:3b` available in Ollama.
- `ffmpeg` installed.
- Piper executable available as `piper` or `~/.local/bin/piper`.
- Local models in the paths described by `local_voice_assistant/models/README.md`.

Health check:

```bash
curl http://127.0.0.1:8088/health
```

Main endpoints:

- `POST /voice-ogg`: audio in, OGG TTS out.
- `POST /chat`: text test endpoint.
- `POST /tts`: text to OGG TTS.
