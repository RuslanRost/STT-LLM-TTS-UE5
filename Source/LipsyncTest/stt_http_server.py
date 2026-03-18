import base64
import ctypes
import json
import os
import threading
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HOST = "127.0.0.1"
PORT = 8083
PROJECT_DIR = Path(__file__).resolve().parents[2]
DEFAULT_VOSK_DIR = PROJECT_DIR / "Vosk"
DEFAULT_VOSK_DLL = DEFAULT_VOSK_DIR / "libvosk.dll"
DEFAULT_MODEL_DIR = PROJECT_DIR / "Models" / "vosk-model-ru-0.42"


def log_event(event, **fields):
    timestamp = datetime.now(timezone.utc).isoformat()
    parts = [f"event={event}"]
    for key, value in fields.items():
        if value is None:
            continue
        parts.append(f"{key}={value!r}")
    print(f"[{timestamp}] " + " ".join(parts), flush=True)


class VoskRecognizerManager:
    def __init__(self):
        self._dll = None
        self._model = None
        self._loaded = False
        self._status = "not_loaded"
        self._model_path = str(DEFAULT_MODEL_DIR)
        self._dll_path = str(DEFAULT_VOSK_DLL)

    def _configure_dll_search_path(self, dll_path):
        dll_dir = str(Path(dll_path).parent)
        os.environ["PATH"] = dll_dir + os.pathsep + os.environ.get("PATH", "")
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(dll_dir)

    def load(self):
        if self._loaded:
            return

        dll_path = Path(self._dll_path)
        model_path = Path(self._model_path)
        if not dll_path.exists():
            raise RuntimeError(f"Vosk DLL not found: {dll_path}")
        if not model_path.exists():
            raise RuntimeError(f"Vosk model not found: {model_path}")

        self._configure_dll_search_path(dll_path)
        self._dll = ctypes.CDLL(str(dll_path))

        self._dll.vosk_model_new.argtypes = [ctypes.c_char_p]
        self._dll.vosk_model_new.restype = ctypes.c_void_p
        self._dll.vosk_model_free.argtypes = [ctypes.c_void_p]
        self._dll.vosk_model_free.restype = None
        self._dll.vosk_recognizer_new.argtypes = [ctypes.c_void_p, ctypes.c_float]
        self._dll.vosk_recognizer_new.restype = ctypes.c_void_p
        self._dll.vosk_recognizer_new_grm.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_char_p]
        self._dll.vosk_recognizer_new_grm.restype = ctypes.c_void_p
        self._dll.vosk_recognizer_free.argtypes = [ctypes.c_void_p]
        self._dll.vosk_recognizer_free.restype = None
        self._dll.vosk_recognizer_accept_waveform.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        self._dll.vosk_recognizer_accept_waveform.restype = ctypes.c_int
        self._dll.vosk_recognizer_result.argtypes = [ctypes.c_void_p]
        self._dll.vosk_recognizer_result.restype = ctypes.c_char_p
        self._dll.vosk_recognizer_partial_result.argtypes = [ctypes.c_void_p]
        self._dll.vosk_recognizer_partial_result.restype = ctypes.c_char_p
        self._dll.vosk_recognizer_final_result.argtypes = [ctypes.c_void_p]
        self._dll.vosk_recognizer_final_result.restype = ctypes.c_char_p
        self._dll.vosk_recognizer_set_words.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self._dll.vosk_recognizer_set_words.restype = None

        self._model = self._dll.vosk_model_new(str(model_path).encode("utf-8"))
        if not self._model:
            raise RuntimeError(f"Failed to create Vosk model: {model_path}")

        self._loaded = True
        self._status = "ready"
        log_event("stt.model.loaded", dll_path=str(dll_path), model_path=str(model_path))

    def status(self):
        return {
            "status": self._status,
            "dll_path": self._dll_path,
            "model_path": self._model_path,
            "loaded": self._loaded,
        }

    def create_session(self, payload):
        self.load()
        return STTSession(self, payload)

    def _downmix_to_mono_pcm16(self, audio_bytes, num_channels):
        if num_channels <= 1:
            return audio_bytes

        sample_count = len(audio_bytes) // 2
        if sample_count % num_channels != 0:
            sample_count -= sample_count % num_channels
            audio_bytes = audio_bytes[: sample_count * 2]

        samples = memoryview(audio_bytes).cast("h")
        mono = bytearray()
        for frame_start in range(0, len(samples), num_channels):
            frame = samples[frame_start : frame_start + num_channels]
            mixed = int(sum(frame) / num_channels)
            mono.extend(int(mixed).to_bytes(2, byteorder="little", signed=True))
        return bytes(mono)

    def transcribe_pcm(self, payload):
        self.load()

        audio_base64 = payload.get("audio_base64", "")
        request_id = payload.get("request_id", "")
        client_id = payload.get("client_id", "")
        sample_rate = int(payload.get("sample_rate", 16000))
        num_channels = int(payload.get("num_channels", 1))
        enable_words = bool(payload.get("enable_words", False))
        grammar_phrases = payload.get("grammar_phrases", [])

        if not audio_base64:
            raise RuntimeError("audio_base64 is empty")
        if sample_rate <= 0:
            raise RuntimeError("sample_rate must be positive")
        if num_channels <= 0:
            raise RuntimeError("num_channels must be positive")

        audio_bytes = base64.b64decode(audio_base64)
        if len(audio_bytes) < 2:
            raise RuntimeError("Audio buffer is too small")

        pcm_mono = self._downmix_to_mono_pcm16(audio_bytes, num_channels)
        grammar_json = None
        if isinstance(grammar_phrases, list) and grammar_phrases:
            grammar_json = json.dumps(grammar_phrases, ensure_ascii=False)

        if grammar_json:
            recognizer = self._dll.vosk_recognizer_new_grm(self._model, float(sample_rate), grammar_json.encode("utf-8"))
        else:
            recognizer = self._dll.vosk_recognizer_new(self._model, float(sample_rate))
        if not recognizer:
            raise RuntimeError("Failed to create Vosk recognizer")

        try:
            self._dll.vosk_recognizer_set_words(recognizer, 1 if enable_words else 0)
            partial_text = ""
            chunk_size = 4000
            for offset in range(0, len(pcm_mono), chunk_size):
                chunk = pcm_mono[offset : offset + chunk_size]
                accepted = self._dll.vosk_recognizer_accept_waveform(recognizer, chunk, len(chunk))
                if accepted:
                    interim = self._dll.vosk_recognizer_result(recognizer)
                else:
                    interim = self._dll.vosk_recognizer_partial_result(recognizer)
                if interim:
                    interim_json = json.loads(interim.decode("utf-8"))
                    partial_text = interim_json.get("text", "") or interim_json.get("partial", partial_text)

            final_raw = self._dll.vosk_recognizer_final_result(recognizer)
            if not final_raw:
                raise RuntimeError("Vosk returned empty final result")

            final_json = json.loads(final_raw.decode("utf-8"))
            text = final_json.get("text", "").strip()
            return {
                "ok": True,
                "text": text,
                "partial": partial_text,
                "request_id": request_id,
                "client_id": client_id,
                "sample_rate": sample_rate,
                "num_channels": 1,
                "result": final_json,
            }
        finally:
            self._dll.vosk_recognizer_free(recognizer)


STT_MANAGER = VoskRecognizerManager()


class STTSession:
    def __init__(self, manager, payload):
        self.manager = manager
        self.session_id = str(uuid.uuid4())
        self.client_id = payload.get("client_id", "")
        self.request_id = payload.get("request_id", "")
        self.sample_rate = int(payload.get("sample_rate", 16000))
        self.num_channels = int(payload.get("num_channels", 1))
        self.enable_words = bool(payload.get("enable_words", False))
        grammar_phrases = payload.get("grammar_phrases", [])
        self.grammar_json = json.dumps(grammar_phrases, ensure_ascii=False) if isinstance(grammar_phrases, list) and grammar_phrases else None
        self.lock = threading.Lock()
        self.recognizer = self._create_recognizer()
        if not self.recognizer:
            raise RuntimeError("Failed to create Vosk recognizer")
        self.manager._dll.vosk_recognizer_set_words(self.recognizer, 1 if self.enable_words else 0)
        self.total_audio_bytes = 0

    def _create_recognizer(self):
        if self.grammar_json:
            return self.manager._dll.vosk_recognizer_new_grm(self.manager._model, float(self.sample_rate), self.grammar_json.encode("utf-8"))
        return self.manager._dll.vosk_recognizer_new(self.manager._model, float(self.sample_rate))

    def close(self):
        with self.lock:
            if self.recognizer:
                self.manager._dll.vosk_recognizer_free(self.recognizer)
                self.recognizer = None

    def _downmix_to_mono_pcm16(self, audio_bytes):
        if self.num_channels <= 1:
            return audio_bytes

        sample_count = len(audio_bytes) // 2
        if sample_count % self.num_channels != 0:
            sample_count -= sample_count % self.num_channels
            audio_bytes = audio_bytes[: sample_count * 2]

        samples = memoryview(audio_bytes).cast("h")
        mono = bytearray()
        for frame_start in range(0, len(samples), self.num_channels):
            frame = samples[frame_start : frame_start + self.num_channels]
            mixed = int(sum(frame) / self.num_channels)
            mono.extend(int(mixed).to_bytes(2, byteorder="little", signed=True))
        return bytes(mono)

    def append_audio(self, audio_base64):
        if not audio_base64:
            return []

        audio_bytes = base64.b64decode(audio_base64)
        pcm_mono = self._downmix_to_mono_pcm16(audio_bytes)
        events = []
        with self.lock:
            if not self.recognizer:
                raise RuntimeError("Session recognizer is closed")
            self.total_audio_bytes += len(audio_bytes)
            chunk_size = 4000
            for offset in range(0, len(pcm_mono), chunk_size):
                chunk = pcm_mono[offset : offset + chunk_size]
                accepted = self.manager._dll.vosk_recognizer_accept_waveform(self.recognizer, chunk, len(chunk))
                if accepted:
                    result_raw = self.manager._dll.vosk_recognizer_result(self.recognizer)
                    if result_raw:
                        result_json = json.loads(result_raw.decode("utf-8"))
                        text = result_json.get("text", "")
                        if text:
                            events.append({"type": "final", "text": text, "result": result_json})
                else:
                    partial_raw = self.manager._dll.vosk_recognizer_partial_result(self.recognizer)
                    if partial_raw:
                        partial_json = json.loads(partial_raw.decode("utf-8"))
                        partial_text = partial_json.get("partial", "")
                        if partial_text:
                            events.append({"type": "partial", "text": partial_text, "result": partial_json})

        return events

    def request_final(self):
        with self.lock:
            if not self.recognizer:
                return []
            final_raw = self.manager._dll.vosk_recognizer_final_result(self.recognizer)
            if not final_raw:
                return []
            final_json = json.loads(final_raw.decode("utf-8"))
            text = final_json.get("text", "")
            if not text:
                return []

        return [{"type": "final", "text": text, "result": final_json}]


SESSIONS = {}
SESSIONS_LOCK = threading.Lock()


def get_session(session_id):
    with SESSIONS_LOCK:
        return SESSIONS.get(session_id)


def create_session(payload):
    session = STT_MANAGER.create_session(payload)
    with SESSIONS_LOCK:
        SESSIONS[session.session_id] = session
    return session


def remove_session(session_id):
    with SESSIONS_LOCK:
        session = SESSIONS.pop(session_id, None)
    if session:
        session.close()
    return session


class STTHttpHandler(BaseHTTPRequestHandler):
    server_version = "STTHttpServer/1.0"

    def log_message(self, format, *args):
        pass

    def _send_json(self, status_code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        raw_body = self.rfile.read(length).decode("utf-8")
        return json.loads(raw_body)

    def do_GET(self):
        if self.path == "/ping":
            self._send_json(
                200,
                {
                    "ok": True,
                    "message": "pong",
                    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                    "stt": STT_MANAGER.status(),
                },
            )
            return

        if self.path == "/status":
            self._send_json(
                200,
                {
                    "ok": True,
                    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                    "stt": STT_MANAGER.status(),
                    "active_sessions": len(SESSIONS),
                },
            )
            return

        self._send_json(404, {"ok": False, "error": "Route not found", "path": self.path})

    def do_POST(self):
        if self.path == "/transcribe_pcm":
            try:
                payload = self._read_json_body()
                result = STT_MANAGER.transcribe_pcm(payload)
                self._send_json(200, result)
            except Exception as exc:
                self._send_json(
                    500,
                    {
                        "ok": False,
                        "error": str(exc),
                        "stt": STT_MANAGER.status(),
                    },
                )
            return

        if self.path == "/start_session":
            try:
                payload = self._read_json_body()
                session = create_session(payload)
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "session_id": session.session_id,
                        "sample_rate": session.sample_rate,
                        "num_channels": session.num_channels,
                        "enable_words": session.enable_words,
                    },
                )
            except Exception as exc:
                self._send_json(500, {"ok": False, "error": str(exc), "stt": STT_MANAGER.status()})
            return

        if self.path == "/append_audio":
            try:
                payload = self._read_json_body()
                session = get_session(payload.get("session_id", ""))
                if not session:
                    raise RuntimeError("Invalid session_id")
                events = session.append_audio(payload.get("audio_base64", ""))
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "session_id": session.session_id,
                        "events": events,
                    },
                )
            except Exception as exc:
                self._send_json(500, {"ok": False, "error": str(exc)})
            return

        if self.path == "/request_final":
            try:
                payload = self._read_json_body()
                session = get_session(payload.get("session_id", ""))
                if not session:
                    raise RuntimeError("Invalid session_id")
                events = session.request_final()
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "session_id": session.session_id,
                        "events": events,
                    },
                )
            except Exception as exc:
                self._send_json(500, {"ok": False, "error": str(exc)})
            return

        if self.path == "/stop_session":
            try:
                payload = self._read_json_body()
                session_id = payload.get("session_id", "")
                session = get_session(session_id)
                events = session.request_final() if session else []
                remove_session(session_id)
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "session_id": session_id,
                        "events": events,
                    },
                )
            except Exception as exc:
                self._send_json(500, {"ok": False, "error": str(exc)})
            return

        self._send_json(404, {"ok": False, "error": "Route not found", "path": self.path})


def main():
    log_event(
        "stt_proxy.boot",
        host=HOST,
        port=PORT,
        project_dir=str(PROJECT_DIR),
        dll_path=str(DEFAULT_VOSK_DLL),
        model_path=str(DEFAULT_MODEL_DIR),
    )
    STT_MANAGER.load()
    httpd = ThreadingHTTPServer((HOST, PORT), STTHttpHandler)
    log_event(
        "stt_proxy.ready",
        url=f"http://{HOST}:{PORT}",
        routes="GET /ping, GET /status, POST /transcribe_pcm, POST /start_session, POST /append_audio, POST /request_final, POST /stop_session",
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        with SESSIONS_LOCK:
            active_sessions = list(SESSIONS.values())
            SESSIONS.clear()
        for session in active_sessions:
            session.close()
        httpd.server_close()


if __name__ == "__main__":
    main()
