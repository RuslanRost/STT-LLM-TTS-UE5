import base64
import json
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HOST = "127.0.0.1"
PORT = 8081
PROJECT_DIR = Path(__file__).resolve().parents[2]
DEFAULT_LLM_HOST = "127.0.0.1"
DEFAULT_LLM_PORT = 8080
DEFAULT_LLM_EXE = PROJECT_DIR / "Llama" / "llama-server.exe"
DEFAULT_LLM_MODEL = PROJECT_DIR / "Models" / "qwen2.5-3b-instruct-q4_k_m.gguf"
DEFAULT_TTS_EXE = PROJECT_DIR / "Piper" / "piper.exe"
DEFAULT_TTS_MODEL = PROJECT_DIR / "Models" / "tts" / "ru_RU-denis-medium.onnx"
AUTO_START_LLM_ON_BOOT = True


def log_event(event, **fields):
    timestamp = datetime.now(timezone.utc).isoformat()
    thread_name = threading.current_thread().name
    parts = [f"event={event}", f"thread={thread_name}"]
    for key, value in fields.items():
        if value is None:
            continue
        parts.append(f"{key}={value!r}")
    print(f"[{timestamp}] " + " ".join(parts), flush=True)


class LLMProcessManager:
    def __init__(self):
        self._lock = threading.Lock()
        self._prompt_lock = threading.Lock()
        self._process = None
        self._config = {
            "server_exe": str(DEFAULT_LLM_EXE),
            "model_path": str(DEFAULT_LLM_MODEL),
            "host": DEFAULT_LLM_HOST,
            "port": DEFAULT_LLM_PORT,
            "context_size": 2048,
            "gpu_layers": 35,
            "timeout": 30.0,
        }

    def _llm_base_url(self):
        return f"http://{self._config['host']}:{self._config['port']}"

    def _health_urls(self):
        base_url = self._llm_base_url()
        return [f"{base_url}/health", f"{base_url}/v1/models"]

    def is_running(self):
        with self._lock:
            return self._process is not None and self._process.poll() is None

    def is_ready(self, timeout=1.5):
        for url in self._health_urls():
            try:
                request = urllib.request.Request(url, method="GET")
                with urllib.request.urlopen(request, timeout=timeout) as response:
                    if 200 <= response.status < 300:
                        log_event("llm.health.ok", url=url, status=response.status)
                        return True
            except Exception as exc:
                log_event("llm.health.fail", url=url, error=str(exc))
                continue
        return False

    def wait_until_ready(self, timeout=None):
        timeout_value = timeout or self._config["timeout"]
        deadline = time.time() + timeout_value
        attempt = 0
        log_event("llm.wait_ready.begin", timeout=timeout_value, base_url=self._llm_base_url())
        while time.time() < deadline:
            attempt += 1
            if self.is_ready(timeout=1.0):
                log_event("llm.wait_ready.ok", attempts=attempt)
                return True
            time.sleep(0.5)
        log_event("llm.wait_ready.timeout", attempts=attempt, timeout=timeout_value)
        return False

    def start(self, payload):
        with self._lock:
            self._config["server_exe"] = payload.get("server_exe_path", self._config["server_exe"])
            self._config["model_path"] = payload.get("model_path", self._config["model_path"])
            self._config["host"] = payload.get("host", self._config["host"])
            self._config["port"] = int(payload.get("port", self._config["port"]))
            self._config["context_size"] = int(payload.get("context_size", self._config["context_size"]))
            self._config["gpu_layers"] = int(payload.get("gpu_layers", self._config["gpu_layers"]))

            log_event(
                "llm.start.request",
                server_exe=self._config["server_exe"],
                model_path=self._config["model_path"],
                host=self._config["host"],
                port=self._config["port"],
                context_size=self._config["context_size"],
                gpu_layers=self._config["gpu_layers"],
            )

            if self._process is not None and self._process.poll() is None:
                log_event("llm.start.skip", reason="already_running", pid=self._process.pid)
                return False, "LLM server is already running"

            exe_path = Path(self._config["server_exe"])
            model_path = Path(self._config["model_path"])
            if not exe_path.exists():
                log_event("llm.start.error", reason="missing_exe", path=str(exe_path))
                return False, f"llama-server.exe not found: {exe_path}"
            if not model_path.exists():
                log_event("llm.start.error", reason="missing_model", path=str(model_path))
                return False, f"Model not found: {model_path}"

            args = [
                str(exe_path),
                "-m",
                str(model_path),
                "--host",
                self._config["host"],
                "--port",
                str(self._config["port"]),
                "-c",
                str(self._config["context_size"]),
                "-ngl",
                str(self._config["gpu_layers"]),
                "--log-disable",
            ]

            log_event("llm.process.spawn", cwd=str(PROJECT_DIR), args=" ".join(args))
            self._process = subprocess.Popen(
                args,
                cwd=str(PROJECT_DIR),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            log_event("llm.process.spawned", pid=self._process.pid)

        if not self.wait_until_ready():
            log_event("llm.start.error", reason="not_ready_in_time")
            return False, "LLM server started but did not become ready in time"

        log_event("llm.start.ok", pid=self._process.pid, base_url=self._llm_base_url())
        return True, "LLM server is ready"

    def stop(self):
        with self._lock:
            process = self._process
            self._process = None

        if process is None or process.poll() is not None:
            log_event("llm.stop.skip", reason="not_running")
            return False, "LLM server is not running"

        log_event("llm.stop.begin", pid=process.pid)
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            log_event("llm.stop.kill", pid=process.pid)
            process.kill()
            process.wait(timeout=5.0)

        log_event("llm.stop.ok", pid=process.pid, returncode=process.returncode)
        return True, "LLM server stopped"

    def status(self):
        running = self.is_running()
        return {
            "running": running,
            "ready": self.is_ready(timeout=0.75) if running else False,
            "host": self._config["host"],
            "port": self._config["port"],
            "server_exe": self._config["server_exe"],
            "model_path": self._config["model_path"],
            "base_url": self._llm_base_url(),
        }

    def send_prompt(self, payload):
        prompt = payload.get("prompt", "")
        system_prompt = payload.get("system_prompt", "")
        temperature = float(payload.get("temperature", 0.7))
        client_id = payload.get("client_id", "")
        request_id = payload.get("request_id", "")

        log_event(
            "prompt.received",
            client_id=client_id,
            request_id=request_id,
            prompt_chars=len(prompt),
            system_prompt_chars=len(system_prompt),
            temperature=temperature,
        )

        if not self.is_running():
            log_event("prompt.autostart", client_id=client_id, request_id=request_id)
            started, message = self.start({})
            if not started:
                log_event("prompt.autostart.error", client_id=client_id, request_id=request_id, error=message)
                raise RuntimeError(message)

        if not self.wait_until_ready():
            log_event("prompt.error", client_id=client_id, request_id=request_id, error="llm_not_ready")
            raise RuntimeError("LLM server is not ready")

        if not prompt:
            log_event("prompt.error", client_id=client_id, request_id=request_id, error="empty_prompt")
            raise RuntimeError("Prompt is empty")

        messages = []
        if system_prompt:
            messages.append({"role": "system", "content": system_prompt})
        messages.append({"role": "user", "content": prompt})

        upstream_payload = {
            "model": "local",
            "messages": messages,
            "temperature": temperature,
            "stream": False,
        }

        request = urllib.request.Request(
            f"{self._llm_base_url()}/v1/chat/completions",
            data=json.dumps(upstream_payload).encode("utf-8"),
            headers={"Content-Type": "application/json; charset=utf-8"},
            method="POST",
        )

        # Serialize prompt execution against a single llama-server process.
        lock_wait_started = time.perf_counter()
        log_event("prompt.queue.wait", client_id=client_id, request_id=request_id)
        with self._prompt_lock:
            lock_wait_elapsed = time.perf_counter() - lock_wait_started
            log_event("prompt.queue.enter", client_id=client_id, request_id=request_id, wait_seconds=f"{lock_wait_elapsed:.3f}")
            upstream_started = time.perf_counter()
            try:
                with urllib.request.urlopen(request, timeout=120.0) as response:
                    raw_body = response.read().decode("utf-8")
                    upstream_json = json.loads(raw_body)
                    upstream_elapsed = time.perf_counter() - upstream_started
                    log_event(
                        "prompt.upstream.ok",
                        client_id=client_id,
                        request_id=request_id,
                        status=response.status,
                        elapsed_seconds=f"{upstream_elapsed:.3f}",
                        response_chars=len(raw_body),
                    )
            except urllib.error.HTTPError as exc:
                error_body = exc.read().decode("utf-8", errors="replace")
                upstream_elapsed = time.perf_counter() - upstream_started
                log_event(
                    "prompt.upstream.http_error",
                    client_id=client_id,
                    request_id=request_id,
                    status=exc.code,
                    elapsed_seconds=f"{upstream_elapsed:.3f}",
                    error_body=error_body,
                )
                raise RuntimeError(f"Upstream HTTP {exc.code}: {error_body}") from exc
            except urllib.error.URLError as exc:
                upstream_elapsed = time.perf_counter() - upstream_started
                log_event(
                    "prompt.upstream.url_error",
                    client_id=client_id,
                    request_id=request_id,
                    elapsed_seconds=f"{upstream_elapsed:.3f}",
                    error=str(exc),
                )
                raise RuntimeError(f"Upstream connection failed: {exc}") from exc

        choices = upstream_json.get("choices", [])
        reply = ""
        if choices:
            reply = choices[0].get("message", {}).get("content", "")

        log_event(
            "prompt.completed",
            client_id=client_id,
            request_id=request_id,
            reply_chars=len(reply),
            choices=len(choices),
        )

        return {
            "ok": True,
            "prompt": prompt,
            "reply": reply,
            "client_id": client_id,
            "request_id": request_id,
            "upstream": upstream_json,
        }


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


LLM_MANAGER = LLMProcessManager()
TTS_MANAGER = TTSProcessManager()


class LLMHttpHandler(BaseHTTPRequestHandler):
    server_version = "LLMHttpServer/1.0"

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
        self.send_header("Connection", "keep-alive")
        self.end_headers()
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
                    "llm": LLM_MANAGER.status(),
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
                    "llm": LLM_MANAGER.status(),
                    "tts": TTS_MANAGER.status(),
                },
            )
            return

        self._send_json(
            404,
            {
                "ok": False,
                "error": "Route not found",
                "path": self.path,
            },
        )

    def do_POST(self):
        log_event("http.request", method="POST", path=self.path, remote=self.address_string())
        if self.path == "/echo":
            payload = self._read_json_body()
            self._send_json(
                200,
                {
                    "ok": True,
                    "route": "/echo",
                    "received": payload,
                    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                },
            )
            return

        if self.path == "/start_llm":
            payload = self._read_json_body()
            started, message = LLM_MANAGER.start(payload)
            self._send_json(
                200 if started else 409,
                {
                    "ok": started,
                    "message": message,
                    "llm": LLM_MANAGER.status(),
                },
            )
            return

        if self.path == "/stop_llm":
            stopped, message = LLM_MANAGER.stop()
            self._send_json(
                200 if stopped else 409,
                {
                    "ok": stopped,
                    "message": message,
                    "llm": LLM_MANAGER.status(),
                },
            )
            return

        if self.path == "/prompt":
            payload = self._read_json_body()
            try:
                result = LLM_MANAGER.send_prompt(payload)
                self._send_json(200, result)
            except Exception as exc:
                log_event(
                    "prompt.failed",
                    client_id=payload.get("client_id", ""),
                    request_id=payload.get("request_id", ""),
                    error=str(exc),
                )
                self._send_json(
                    500,
                    {
                        "ok": False,
                        "error": str(exc),
                        "llm": LLM_MANAGER.status(),
                    },
                )
            return

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

        self._send_json(
            404,
            {
                "ok": False,
                "error": "Route not found",
                "path": self.path,
            },
        )


def main():
    log_event(
        "llm_proxy.boot",
        host=HOST,
        port=PORT,
        project_dir=str(PROJECT_DIR),
        default_llm_exe=str(DEFAULT_LLM_EXE),
        default_llm_model=str(DEFAULT_LLM_MODEL),
        auto_start_llm=AUTO_START_LLM_ON_BOOT,
    )
    if AUTO_START_LLM_ON_BOOT:
        log_event("llm_proxy.autostart.begin")
        started, message = LLM_MANAGER.start({})
        log_event("llm_proxy.autostart.result", ok=started, message=message)

    httpd = ThreadingHTTPServer((HOST, PORT), LLMHttpHandler)
    log_event(
        "llm_proxy.ready",
        url=f"http://{HOST}:{PORT}",
        routes="GET /ping, GET /status, POST /echo, POST /start_llm, POST /stop_llm, POST /prompt, POST /tts_stream",
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log_event("llm_proxy.shutdown.signal", signal="KeyboardInterrupt")
    finally:
        if LLM_MANAGER.is_running():
            LLM_MANAGER.stop()
        httpd.server_close()
        log_event("llm_proxy.shutdown.complete")


if __name__ == "__main__":
    main()
