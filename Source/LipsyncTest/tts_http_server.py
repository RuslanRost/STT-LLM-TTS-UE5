import base64
import json
import subprocess
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HOST = "127.0.0.1"
PORT = 8082
PROJECT_DIR = Path(__file__).resolve().parents[2]
DEFAULT_TTS_EXE = PROJECT_DIR / "Piper" / "piper.exe"
DEFAULT_TTS_MODEL = PROJECT_DIR / "Models" / "tts" / "ru_RU-denis-medium.onnx"


def log_event(event, **fields):
    timestamp = datetime.now(timezone.utc).isoformat()
    thread_name = threading.current_thread().name
    parts = [f"event={event}", f"thread={thread_name}"]
    for key, value in fields.items():
        if value is None:
            continue
        parts.append(f"{key}={value!r}")
    print(f"[{timestamp}] " + " ".join(parts), flush=True)


class TTSProcessManager:
    def __init__(self):
        self._lock = threading.Lock()
        self._busy = False
        self._config = {
            "piper_exe": str(DEFAULT_TTS_EXE),
            "model_path": str(DEFAULT_TTS_MODEL),
            "model_config_path": "",
            "use_cuda": False,
            "speaker_id": -1,
            "length_scale": 1.0,
            "noise_scale": 0.667,
            "noise_w": 0.8,
            "chunk_size": 4096,
        }

    def _resolve_config(self, payload):
        config = dict(self._config)
        config["piper_exe"] = payload.get("piper_exe_path", config["piper_exe"])
        config["model_path"] = payload.get("model_path", config["model_path"])
        config["model_config_path"] = payload.get("model_config_path", config["model_config_path"])
        config["use_cuda"] = bool(payload.get("use_cuda", config["use_cuda"]))
        config["speaker_id"] = int(payload.get("speaker_id", config["speaker_id"]))
        config["length_scale"] = float(payload.get("length_scale", config["length_scale"]))
        config["noise_scale"] = float(payload.get("noise_scale", config["noise_scale"]))
        config["noise_w"] = float(payload.get("noise_w", config["noise_w"]))
        config["chunk_size"] = max(512, int(payload.get("chunk_size", config["chunk_size"])))
        return config

    def _resolve_model_config_path(self, config):
        if config["model_config_path"]:
            return Path(config["model_config_path"])
        return Path(f"{config['model_path']}.json")

    def _load_sample_rate(self, config):
        config_path = self._resolve_model_config_path(config)
        if not config_path.exists():
            raise RuntimeError(f"Model config not found: {config_path}")

        with config_path.open("r", encoding="utf-8") as file:
            root = json.load(file)

        sample_rate = 0
        audio_config = root.get("audio")
        if isinstance(audio_config, dict):
            sample_rate = int(audio_config.get("sample_rate", 0))
        if sample_rate <= 0:
            sample_rate = int(root.get("sample_rate", 0))
        if sample_rate <= 0:
            raise RuntimeError(f"Sample rate not found in model config: {config_path}")
        return sample_rate

    def status(self):
        with self._lock:
            return {
                "busy": self._busy,
                "piper_exe": self._config["piper_exe"],
                "model_path": self._config["model_path"],
                "model_config_path": self._config["model_config_path"] or f"{self._config['model_path']}.json",
                "chunk_size": self._config["chunk_size"],
                "use_cuda": self._config["use_cuda"],
            }

    def stream_speech(self, payload):
        text = payload.get("text", "")
        client_id = payload.get("client_id", "")
        request_id = payload.get("request_id", "")
        if not text:
            raise RuntimeError("Text is empty")

        config = self._resolve_config(payload)
        piper_exe = Path(config["piper_exe"])
        model_path = Path(config["model_path"])
        if not piper_exe.exists():
            raise RuntimeError(f"Piper executable not found: {piper_exe}")
        if not model_path.exists():
            raise RuntimeError(f"TTS model not found: {model_path}")

        sample_rate = self._load_sample_rate(config)
        args = [
            str(piper_exe),
            "--model",
            str(model_path),
            "--output-raw",
        ]
        if config["use_cuda"]:
            args.append("--cuda")
        if config["speaker_id"] >= 0:
            args.extend(["--speaker", str(config["speaker_id"])])
        args.extend(
            [
                "--length-scale",
                str(config["length_scale"]),
                "--noise-scale",
                str(config["noise_scale"]),
                "--noise-w",
                str(config["noise_w"]),
            ]
        )

        lock_wait_started = time.perf_counter()
        log_event("tts.queue.wait", client_id=client_id, request_id=request_id, text_chars=len(text))
        with self._lock:
            lock_wait_elapsed = time.perf_counter() - lock_wait_started
            self._busy = True
            log_event(
                "tts.queue.enter",
                client_id=client_id,
                request_id=request_id,
                wait_seconds=f"{lock_wait_elapsed:.3f}",
                piper_exe=str(piper_exe),
                model_path=str(model_path),
                chunk_size=config["chunk_size"],
                sample_rate=sample_rate,
            )

            process = None
            total_bytes = 0
            chunk_index = 0
            odd_tail = b""
            started_at = time.perf_counter()
            try:
                process = subprocess.Popen(
                    args,
                    cwd=str(PROJECT_DIR),
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    bufsize=0,
                )
                log_event("tts.process.spawned", client_id=client_id, request_id=request_id, pid=process.pid)

                if process.stdin is None or process.stdout is None:
                    raise RuntimeError("Failed to open piper pipes")

                process.stdin.write(text.encode("utf-8"))
                process.stdin.write(b"\n")
                process.stdin.flush()
                process.stdin.close()

                yield {
                    "type": "start",
                    "text": text,
                    "client_id": client_id,
                    "request_id": request_id,
                    "sample_rate": sample_rate,
                    "num_channels": 1,
                    "format": "pcm_s16le",
                    "chunk_size": config["chunk_size"],
                }

                while True:
                    reader = getattr(process.stdout, "read1", None)
                    chunk = reader(config["chunk_size"]) if reader else process.stdout.read(config["chunk_size"])
                    if not chunk:
                        if process.poll() is not None:
                            break
                        time.sleep(0.01)
                        continue

                    if odd_tail:
                        chunk = odd_tail + chunk
                        odd_tail = b""
                    if len(chunk) % 2 != 0:
                        odd_tail = chunk[-1:]
                        chunk = chunk[:-1]
                    if not chunk:
                        continue

                    total_bytes += len(chunk)
                    encoded = base64.b64encode(chunk).decode("ascii")
                    log_event(
                        "tts.chunk",
                        client_id=client_id,
                        request_id=request_id,
                        chunk_index=chunk_index,
                        bytes=len(chunk),
                        total_bytes=total_bytes,
                    )
                    yield {
                        "type": "audio_chunk",
                        "text": text,
                        "client_id": client_id,
                        "request_id": request_id,
                        "sample_rate": sample_rate,
                        "num_channels": 1,
                        "format": "pcm_s16le",
                        "chunk_index": chunk_index,
                        "num_bytes": len(chunk),
                        "chunk_base64": encoded,
                    }
                    chunk_index += 1

                if odd_tail:
                    log_event("tts.chunk.drop_odd_tail", client_id=client_id, request_id=request_id, bytes=len(odd_tail))

                stderr_output = ""
                if process.stderr is not None:
                    stderr_output = process.stderr.read().decode("utf-8", errors="replace").strip()

                return_code = process.wait(timeout=5.0)
                if return_code != 0:
                    log_event(
                        "tts.process.error",
                        client_id=client_id,
                        request_id=request_id,
                        returncode=return_code,
                        stderr=stderr_output,
                    )
                    raise RuntimeError(f"Piper exited with code {return_code}: {stderr_output}")
                if total_bytes == 0:
                    raise RuntimeError("No audio data received from piper")

                elapsed = time.perf_counter() - started_at
                log_event(
                    "tts.completed",
                    client_id=client_id,
                    request_id=request_id,
                    total_bytes=total_bytes,
                    chunks=chunk_index,
                    elapsed_seconds=f"{elapsed:.3f}",
                )
                yield {
                    "type": "end",
                    "text": text,
                    "client_id": client_id,
                    "request_id": request_id,
                    "sample_rate": sample_rate,
                    "num_channels": 1,
                    "format": "pcm_s16le",
                    "chunk_count": chunk_index,
                    "total_bytes": total_bytes,
                    "elapsed_seconds": elapsed,
                }
            finally:
                self._busy = False
                if process is not None and process.poll() is None:
                    log_event("tts.process.terminate", client_id=client_id, request_id=request_id, pid=process.pid)
                    process.terminate()
                    try:
                        process.wait(timeout=2.0)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2.0)


TTS_MANAGER = TTSProcessManager()


class TTSHttpHandler(BaseHTTPRequestHandler):
    server_version = "TTSHttpServer/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        log_event("http.access", remote=self.address_string(), message=format % args)

    def _send_json(self, status_code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        log_event("http.response", method=self.command, path=self.path, status=status_code, bytes=len(body))

    def _read_json_body(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            log_event("http.body.empty", method=self.command, path=self.path)
            return {}

        raw_body = self.rfile.read(length).decode("utf-8")
        try:
            payload = json.loads(raw_body)
            log_event("http.body.json", method=self.command, path=self.path, bytes=length, keys=sorted(payload.keys()))
            return payload
        except json.JSONDecodeError:
            log_event("http.body.invalid_json", method=self.command, path=self.path, bytes=length)
            return {"_raw": raw_body}

    def _send_sse_headers(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        log_event("http.sse.open", method=self.command, path=self.path)

    def _send_sse_event(self, payload):
        body = f"data: {json.dumps(payload, ensure_ascii=False)}\n\n".encode("utf-8")
        self.wfile.write(body)
        self.wfile.flush()
        log_event("http.sse.event", method=self.command, path=self.path, event_type=payload.get("type"), bytes=len(body))

    def do_GET(self):
        log_event("http.request", method="GET", path=self.path, remote=self.address_string())
        if self.path == "/ping":
            self._send_json(
                200,
                {
                    "ok": True,
                    "message": "pong",
                    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                    "tts": TTS_MANAGER.status(),
                },
            )
            return

        if self.path == "/status":
            self._send_json(
                200,
                {
                    "ok": True,
                    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                    "tts": TTS_MANAGER.status(),
                },
            )
            return

        self._send_json(404, {"ok": False, "error": "Route not found", "path": self.path})

    def do_POST(self):
        log_event("http.request", method="POST", path=self.path, remote=self.address_string())
        if self.path == "/tts_stream":
            payload = self._read_json_body()
            client_id = payload.get("client_id", "")
            request_id = payload.get("request_id", "")
            try:
                self._send_sse_headers()
                for event in TTS_MANAGER.stream_speech(payload):
                    self._send_sse_event(event)
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
                log_event("http.sse.done", method=self.command, path=self.path, client_id=client_id, request_id=request_id)
            except BrokenPipeError:
                log_event("tts.client_disconnected", client_id=client_id, request_id=request_id)
            except Exception as exc:
                log_event("tts.failed", client_id=client_id, request_id=request_id, error=str(exc))
                try:
                    self._send_sse_event(
                        {
                            "type": "error",
                            "client_id": client_id,
                            "request_id": request_id,
                            "error": str(exc),
                        }
                    )
                    self.wfile.write(b"data: [DONE]\n\n")
                    self.wfile.flush()
                except BrokenPipeError:
                    log_event("tts.error_pipe_closed", client_id=client_id, request_id=request_id)
            return

        self._send_json(404, {"ok": False, "error": "Route not found", "path": self.path})


def main():
    log_event(
        "tts_proxy.boot",
        host=HOST,
        port=PORT,
        project_dir=str(PROJECT_DIR),
        default_tts_exe=str(DEFAULT_TTS_EXE),
        default_tts_model=str(DEFAULT_TTS_MODEL),
    )
    httpd = ThreadingHTTPServer((HOST, PORT), TTSHttpHandler)
    log_event(
        "tts_proxy.ready",
        url=f"http://{HOST}:{PORT}",
        routes="GET /ping, GET /status, POST /tts_stream",
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log_event("tts_proxy.shutdown.signal", signal="KeyboardInterrupt")
    finally:
        httpd.server_close()
        log_event("tts_proxy.shutdown.complete")


if __name__ == "__main__":
    main()
