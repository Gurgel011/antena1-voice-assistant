#!/usr/bin/env python3
import asyncio
import json
import os
import subprocess
import tempfile
import time
import wave
import audioop
from dataclasses import dataclass, field
from http import HTTPStatus

import opuslib
import websockets

import server as local_voice


HOST = "0.0.0.0"
PORT = 8089
PATH = "/xiaozhi"
INPUT_SAMPLE_RATE = 16000
OUTPUT_SAMPLE_RATE = 24000
CHANNELS = 1
FRAME_DURATION_MS = 60
INPUT_FRAME_SAMPLES = INPUT_SAMPLE_RATE * FRAME_DURATION_MS // 1000
OUTPUT_FRAME_SAMPLES = OUTPUT_SAMPLE_RATE * FRAME_DURATION_MS // 1000
SILENCE_TIMEOUT_SECONDS = 0.8
MIN_UTTERANCE_SECONDS = 0.65
MIN_RMS = 70
MAX_UTTERANCE_SECONDS = 5.0
VOICE_START_RMS = 180
VOICE_CONTINUE_RMS = 85
DEBUG_AUDIO_EVERY_FRAMES = 100
AUTO_START_RADIO = os.environ.get("XIAOZHI_AUTO_START_RADIO") == "1"
COMMANDS_WITH_ACTION = {
    "radio_start": "antena1.radio.tocar",
    "radio_stop": "antena1.radio.parar",
    "volume_up": "antena1.radio.aumentar_volume",
    "volume_down": "antena1.radio.diminuir_volume",
}
COMMAND_ANSWERS = {
    "radio_start": "Claro, vou tocar a Rádio Antena 1 agora.",
    "radio_stop": "Certo, vou parar a Rádio Antena 1.",
    "volume_up": "Volume aumentado.",
    "volume_down": "Volume diminuído.",
}
ANTENA1_STREAM_URL = "https://antenaone.crossradio.com.br/stream/1;"
DEFAULT_VOLUME_PERCENT = 120
VOLUME_STEP_PERCENT = 35
OPUS_BITRATE = 64000
OPUS_COMPLEXITY = 6


@dataclass
class Session:
    session_id: str
    decoder: opuslib.Decoder = field(default_factory=lambda: opuslib.Decoder(INPUT_SAMPLE_RATE, CHANNELS))
    pcm: bytearray = field(default_factory=bytearray)
    listening: bool = False
    processing: bool = False
    speech_started: bool = False
    last_voice_at: float = 0.0
    frame_count: int = 0
    mcp_id: int = 1
    auto_started: bool = False
    radio_task: asyncio.Task | None = None
    radio_process: asyncio.subprocess.Process | None = None
    volume_percent: int = DEFAULT_VOLUME_PERCENT
    tts_active: bool = False
    duck_until: float = 0.0


def clamp(value: int, minimum: int, maximum: int) -> int:
    return max(minimum, min(maximum, value))


def volume_scale(session: Session) -> float:
    scale = session.volume_percent / 100.0
    if time.monotonic() < session.duck_until:
        scale *= 0.12
    return scale


def duck_radio_for_voice(session: Session, seconds: float = 2.0) -> None:
    session.duck_until = max(session.duck_until, time.monotonic() + seconds)


def detect_command(text: str) -> str:
    clean = local_voice.normalize(text)
    words = clean.split()
    compact = clean.replace(" ", "")

    if local_voice.wants_radio_stop(clean) or (
        local_voice.has_any_word(clean, ["parar", "pare", "para", "pausar", "pause", "desligar"])
        and local_voice.has_any_word(clean, ["radio", "antena", "tocar", "som", "musica"])
    ):
        return "radio_stop"

    if local_voice.wants_radio_playback(clean) or local_voice.is_standalone_antena1_play_request(clean):
        return "radio_start"

    volume_words = (
        "volume" in clean
        or "som" in clean
        or "audio" in clean
        or "alto" in clean
        or "baixo" in clean
        or "radio" in clean
        or "antena" in clean
    )
    up_words = ["aumenta", "aumentar", "aumente", "aumento", "almenta", "sobe", "sube", "subir", "alto"]
    down_words = ["diminui", "diminuir", "diminua", "diminuiu", "abaixa", "abaixar", "baixa", "baixar", "baixo", "menos", "reduz"]
    if (
        volume_words
        or len(words) <= 5
        or any(word in compact for word in up_words)
    ) and (
        local_voice.has_any_word(clean, up_words)
        or "mais alto" in clean
        or any(word in compact for word in ["aumenta", "aumentar", "aumente", "almenta"])
    ):
        return "volume_up"
    if (
        volume_words
        or "pouquinho" in clean
        or "um pouco" in clean
        or len(words) <= 5
        or any(word in compact for word in down_words)
    ) and (
        local_voice.has_any_word(clean, down_words)
        or "mais baixo" in clean
        or any(word in compact for word in ["diminui", "diminuir", "diminua", "abaixa", "baixar"])
    ):
        return "volume_down"
    return local_voice.detect_command(text)


def make_opus_encoder() -> opuslib.Encoder:
    encoder = opuslib.Encoder(OUTPUT_SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_AUDIO)
    encoder.bitrate = OPUS_BITRATE
    encoder.complexity = OPUS_COMPLEXITY
    encoder.vbr = True
    return encoder


def write_wav(path: str, pcm: bytes, sample_rate: int) -> None:
    with wave.open(path, "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)


def synthesize_to_opus_frames(text: str) -> list[bytes]:
    if not os.path.isfile(local_voice.PIPER_MODEL):
        raise RuntimeError(f"Modelo Piper nao encontrado: {local_voice.PIPER_MODEL}")
    if not os.path.isfile(local_voice.PIPER_BIN):
        raise RuntimeError(f"Executavel Piper nao encontrado: {local_voice.PIPER_BIN}")

    with tempfile.TemporaryDirectory(prefix="xiaozhi_ws_tts_") as tmp:
        wav_path = os.path.join(tmp, "answer.wav")
        pcm_path = os.path.join(tmp, "answer.s16le")
        subprocess.run(
            [
                local_voice.PIPER_BIN,
                "--model",
                local_voice.PIPER_MODEL,
                "--length_scale",
                "1.12",
                "--noise_scale",
                "0.55",
                "--noise_w",
                "0.65",
                "--output_file",
                wav_path,
            ],
            input=local_voice.prepare_tts_text(text).encode("utf-8"),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            check=True,
        )
        subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-loglevel",
                "error",
                "-i",
                wav_path,
                "-ac",
                "1",
                "-ar",
                str(OUTPUT_SAMPLE_RATE),
                "-sample_fmt",
                "s16",
                "-f",
                "s16le",
                pcm_path,
            ],
            check=True,
        )
        with open(pcm_path, "rb") as f:
            pcm = f.read()

    frame_bytes = OUTPUT_FRAME_SAMPLES * 2
    encoder = make_opus_encoder()
    frames = []
    for offset in range(0, len(pcm), frame_bytes):
        chunk = pcm[offset:offset + frame_bytes]
        if len(chunk) < frame_bytes:
            chunk += b"\x00" * (frame_bytes - len(chunk))
        frames.append(encoder.encode(chunk, OUTPUT_FRAME_SAMPLES))
    return frames


async def send_json(ws, payload: dict) -> None:
    await ws.send(json.dumps(payload, ensure_ascii=False))


async def stop_radio_stream(ws, session: Session) -> None:
    task = session.radio_task
    session.radio_task = None
    if task is not None and not task.done():
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
    proc = session.radio_process
    session.radio_process = None
    if proc is not None and proc.returncode is None:
        proc.terminate()
        try:
            await asyncio.wait_for(proc.wait(), timeout=2)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
    try:
        await send_json(ws, {"session_id": session.session_id, "type": "tts", "state": "stop"})
    except Exception:
        pass


async def radio_stream_task(ws, session: Session) -> None:
    frame_bytes = OUTPUT_FRAME_SAMPLES * 2
    encoder = make_opus_encoder()
    proc = await asyncio.create_subprocess_exec(
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-reconnect",
        "1",
        "-reconnect_streamed",
        "1",
        "-reconnect_delay_max",
        "2",
        "-i",
        ANTENA1_STREAM_URL,
        "-ac",
        "1",
        "-ar",
        str(OUTPUT_SAMPLE_RATE),
        "-sample_fmt",
        "s16",
        "-f",
        "s16le",
        "pipe:1",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    session.radio_process = proc
    print("RADIO stream started via ffmpeg", flush=True)
    try:
        await send_json(ws, {"session_id": session.session_id, "type": "tts", "state": "start"})
        await send_json(ws, {
            "session_id": session.session_id,
            "type": "tts",
            "state": "sentence_start",
            "text": "Rádio Antena 1 ao vivo",
        })
        sent_frames = 0
        while proc.returncode is None:
            chunk = await proc.stdout.readexactly(frame_bytes)
            if session.tts_active:
                await asyncio.sleep(FRAME_DURATION_MS / 1000)
                continue
            scale = volume_scale(session)
            if scale != 1.0:
                chunk = audioop.mul(chunk, 2, scale)
            await ws.send(encoder.encode(chunk, OUTPUT_FRAME_SAMPLES))
            sent_frames += 1
            if sent_frames % 100 == 0:
                rms = audioop.rms(chunk, 2)
                print(
                    f"RADIO streaming frames={sent_frames} rms={rms} volume={session.volume_percent}",
                    flush=True,
                )
            await asyncio.sleep(FRAME_DURATION_MS / 1000)
    except asyncio.IncompleteReadError:
        print("RADIO stream ended", flush=True)
    except asyncio.CancelledError:
        print("RADIO stream cancelled", flush=True)
        raise
    except Exception as exc:
        print(f"RADIO stream failed: {exc}", flush=True)
    finally:
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=2)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
        session.radio_process = None
        if session.radio_task is asyncio.current_task():
            session.radio_task = None
        try:
            await send_json(ws, {"session_id": session.session_id, "type": "tts", "state": "stop"})
        except Exception:
            pass


async def start_radio_stream(ws, session: Session) -> None:
    await stop_radio_stream(ws, session)
    session.radio_task = asyncio.create_task(radio_stream_task(ws, session))


async def adjust_volume(ws, session: Session, delta: int) -> None:
    session.volume_percent = clamp(session.volume_percent + delta, 0, 200)
    print(f"RADIO volume={session.volume_percent}", flush=True)
    await send_json(ws, {
        "session_id": session.session_id,
        "type": "stt",
        "text": f"Volume {session.volume_percent}%",
    })


async def send_tts(ws, session: Session, text: str) -> None:
    session.tts_active = True
    duck_radio_for_voice(session, 1.0)
    await send_json(ws, {"session_id": session.session_id, "type": "tts", "state": "start"})
    await send_json(ws, {
        "session_id": session.session_id,
        "type": "tts",
        "state": "sentence_start",
        "text": text,
    })
    try:
        frames = await asyncio.to_thread(synthesize_to_opus_frames, text)
        for frame in frames:
            await ws.send(frame)
            await asyncio.sleep(FRAME_DURATION_MS / 1000)
    except Exception as exc:
        print(f"TTS failed: {exc}", flush=True)
    finally:
        await send_json(ws, {"session_id": session.session_id, "type": "tts", "state": "stop"})
        session.tts_active = False


async def call_tool(ws, session: Session, tool_name: str) -> None:
    request_id = session.mcp_id
    session.mcp_id += 1
    await send_json(ws, {
        "session_id": session.session_id,
        "type": "mcp",
        "payload": {
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "name": tool_name,
                "arguments": {},
            },
            "id": request_id,
        },
    })


async def auto_start_radio_once(ws, session: Session) -> None:
    await asyncio.sleep(2)
    if session.auto_started:
        return
    session.auto_started = True
    print("AUTO radio_start", flush=True)
    await start_radio_stream(ws, session)


async def handle_utterance(ws, session: Session, pcm: bytes) -> None:
    duration = len(pcm) / 2 / INPUT_SAMPLE_RATE
    if duration < MIN_UTTERANCE_SECONDS:
        print(f"Ignoring short utterance duration={duration:.2f}s", flush=True)
        return
    rms = audioop.rms(pcm, 2)
    if rms < MIN_RMS:
        print(f"Ignoring quiet utterance duration={duration:.2f}s rms={rms}", flush=True)
        return

    session.processing = True
    try:
        with tempfile.TemporaryDirectory(prefix="xiaozhi_ws_asr_") as tmp:
            wav_path = os.path.join(tmp, "input.wav")
            write_wav(wav_path, pcm, INPUT_SAMPLE_RATE)
            transcript = await asyncio.to_thread(local_voice.transcribe_with_whisper, wav_path)
            if not transcript:
                transcript = await asyncio.to_thread(local_voice.transcribe_with_vosk, wav_path)

        transcript = transcript.strip()
        print(f"STT transcript={transcript!r}", flush=True)
        if not transcript:
            print("Ignoring empty transcript", flush=True)
            return

        await send_json(ws, {"session_id": session.session_id, "type": "stt", "text": transcript})
        command = detect_command(transcript)

        if command not in COMMANDS_WITH_ACTION:
            print(
                f"Ignoring transcript without radio command command={command!r} "
                f"transcript={transcript!r}",
                flush=True,
            )
            return

        if command == "radio_start":
            print("RADIO command=start", flush=True)
            session.pcm.clear()
            session.speech_started = False
            await send_tts(ws, session, COMMAND_ANSWERS[command])
            await start_radio_stream(ws, session)
        elif command == "radio_stop":
            print("RADIO command=stop", flush=True)
            session.pcm.clear()
            session.speech_started = False
            await stop_radio_stream(ws, session)
            answer = COMMAND_ANSWERS.get(command, "")
            if answer:
                await send_tts(ws, session, answer)
        elif command == "volume_up":
            print("RADIO command=volume_up", flush=True)
            session.pcm.clear()
            session.speech_started = False
            await adjust_volume(ws, session, VOLUME_STEP_PERCENT)
            await send_tts(ws, session, f"Volume aumentado para {session.volume_percent} por cento.")
        elif command == "volume_down":
            print("RADIO command=volume_down", flush=True)
            session.pcm.clear()
            session.speech_started = False
            await adjust_volume(ws, session, -VOLUME_STEP_PERCENT)
            await send_tts(ws, session, f"Volume diminuido para {session.volume_percent} por cento.")
    finally:
        session.processing = False


async def silence_watcher(ws, session: Session) -> None:
    while True:
        await asyncio.sleep(0.2)
        if not session.listening or session.processing or not session.speech_started:
            continue
        utterance_duration = len(session.pcm) / 2 / INPUT_SAMPLE_RATE
        if (
            utterance_duration < MAX_UTTERANCE_SECONDS
            and time.monotonic() - session.last_voice_at < SILENCE_TIMEOUT_SECONDS
        ):
            continue
        pcm = bytes(session.pcm)
        session.pcm.clear()
        session.speech_started = False
        await handle_utterance(ws, session, pcm)


async def handle_connection(ws) -> None:
    session = Session(session_id=f"local-{int(time.time() * 1000) % 100000}")
    watcher = asyncio.create_task(silence_watcher(ws, session))
    print(f"Client connected path={getattr(ws, 'path', '')}", flush=True)
    try:
        async for message in ws:
            if isinstance(message, bytes):
                if not session.listening or session.processing:
                    continue
                try:
                    pcm = session.decoder.decode(message, INPUT_FRAME_SAMPLES)
                    rms = audioop.rms(pcm, 2)
                    session.frame_count += 1

                    if session.frame_count % DEBUG_AUDIO_EVERY_FRAMES == 0:
                        print(
                            f"AUDIO frames={session.frame_count} rms={rms} "
                            f"speech={session.speech_started}",
                            flush=True,
                        )

                    if rms >= VOICE_START_RMS or (
                        session.speech_started and rms >= VOICE_CONTINUE_RMS
                    ):
                        session.speech_started = True
                        session.last_voice_at = time.monotonic()
                        duck_radio_for_voice(session)

                    if session.speech_started:
                        session.pcm.extend(pcm)
                except Exception as exc:
                    print(f"OPUS decode failed: {exc}", flush=True)
                continue

            data = json.loads(message)
            msg_type = data.get("type")
            if msg_type == "hello":
                print(f"HELLO {data}", flush=True)
                await send_json(ws, {
                    "type": "hello",
                    "transport": "websocket",
                    "session_id": session.session_id,
                    "audio_params": {
                        "format": "opus",
                        "sample_rate": OUTPUT_SAMPLE_RATE,
                        "channels": CHANNELS,
                        "frame_duration": FRAME_DURATION_MS,
                    },
                })
            elif msg_type == "listen":
                state = data.get("state")
                print(f"LISTEN state={state} mode={data.get('mode')}", flush=True)
                if state == "start":
                    session.listening = True
                    session.pcm.clear()
                    session.speech_started = False
                    session.frame_count = 0
                    if AUTO_START_RADIO and not session.auto_started:
                        asyncio.create_task(auto_start_radio_once(ws, session))
                elif state == "stop":
                    session.listening = False
                    if session.pcm and not session.processing:
                        pcm = bytes(session.pcm)
                        session.pcm.clear()
                        session.speech_started = False
                        await handle_utterance(ws, session, pcm)
            elif msg_type == "mcp":
                print(f"MCP response {data.get('payload')}", flush=True)
            elif msg_type == "abort":
                session.pcm.clear()
                session.processing = False
                print("ABORT", flush=True)
            else:
                print(f"JSON {data}", flush=True)
    finally:
        await stop_radio_stream(ws, session)
        watcher.cancel()
        print("Client disconnected", flush=True)


async def process_http_request(path, request_headers):
    if path == "/time":
        body = f"{int(time.time())}\n".encode("ascii")
        return (
            HTTPStatus.OK,
            [
                ("Content-Type", "text/plain"),
                ("Content-Length", str(len(body))),
                ("Cache-Control", "no-store"),
            ],
            body,
        )
    return None


async def main() -> None:
    try:
        await asyncio.to_thread(local_voice.get_whisper_model)
    except Exception as exc:
        print(f"Whisper preload failed, Vosk fallback will be used: {exc}", flush=True)

    print(f"Servidor Xiaozhi local em ws://{HOST}:{PORT}{PATH}", flush=True)
    async with websockets.serve(
        handle_connection,
        HOST,
        PORT,
        max_size=None,
        ping_interval=None,
        process_request=process_http_request,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
