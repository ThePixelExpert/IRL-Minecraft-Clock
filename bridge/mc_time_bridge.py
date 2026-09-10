"""
Polls a Minecraft server over RCON for the current daytime tick and serves
it over plain HTTP JSON for the ESP32 clock to poll.

    GET /time -> {"ticks": 6123, "day": 4, "ok": true}

Run:
    cp config.example.json config.json   # fill in your RCON details
    python3 mc_time_bridge.py
"""
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from rcon import RCONClient, RCONError

CONFIG_PATH = Path(__file__).parent / "config.json"
TIME_RE = re.compile(r"(-?\d+)")

state_lock = threading.Lock()
state = {"ticks": 0, "day": 0, "ok": False, "last_updated": 0}


def load_config() -> dict:
    if not CONFIG_PATH.exists():
        raise SystemExit(
            f"Missing {CONFIG_PATH}. Copy config.example.json to config.json and fill it in."
        )
    return json.loads(CONFIG_PATH.read_text())


def poll_loop(cfg: dict) -> None:
    poll_interval = cfg.get("poll_interval_seconds", 2)
    while True:
        try:
            with RCONClient(cfg["rcon_host"], cfg["rcon_port"], cfg["rcon_password"]) as client:
                daytime_resp = client.command("time query daytime")
                day_resp = client.command("time query day")
                ticks_match = TIME_RE.search(daytime_resp)
                day_match = TIME_RE.search(day_resp)
                if ticks_match:
                    with state_lock:
                        state["ticks"] = int(ticks_match.group(1)) % 24000
                        state["day"] = int(day_match.group(1)) if day_match else state["day"]
                        state["ok"] = True
                        state["last_updated"] = time.time()
        except (RCONError, OSError) as exc:
            print(f"[bridge] RCON poll failed: {exc}")
            with state_lock:
                state["ok"] = False
        time.sleep(poll_interval)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/time":
            self.send_response(404)
            self.end_headers()
            return
        with state_lock:
            body = json.dumps(state).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass  # keep stdout quiet; poll_loop already logs errors


def main() -> None:
    cfg = load_config()
    threading.Thread(target=poll_loop, args=(cfg,), daemon=True).start()
    bind_port = cfg.get("bind_port", 5005)
    server = ThreadingHTTPServer(("0.0.0.0", bind_port), Handler)
    print(f"[bridge] serving on :{bind_port}, polling {cfg['rcon_host']}:{cfg['rcon_port']}")
    server.serve_forever()


if __name__ == "__main__":
    main()
