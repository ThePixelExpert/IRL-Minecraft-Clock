"""Minimal Source RCON client (stdlib only) for Minecraft server queries."""
import socket
import struct


class RCONError(Exception):
    pass


class RCONClient:
    def __init__(self, host: str, port: int, password: str, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.password = password
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self._req_id = 0

    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self._send(3, self.password)  # SERVERDATA_AUTH
        req_id, resp_type, _ = self._recv()
        if req_id == -1 or resp_type != 2:  # -1 == auth failed, 2 == AUTH_RESPONSE
            self.sock.close()
            self.sock = None
            raise RCONError("RCON authentication failed (check host/port/password)")

    def close(self) -> None:
        if self.sock:
            self.sock.close()
            self.sock = None

    def command(self, cmd: str) -> str:
        if not self.sock:
            raise RCONError("not connected")
        self._send(2, cmd)  # SERVERDATA_EXECCOMMAND
        _, _, body = self._recv()
        return body

    def _send(self, pkt_type: int, payload: str) -> None:
        self._req_id += 1
        data = payload.encode("utf-8") + b"\x00\x00"
        packet = struct.pack("<ii", self._req_id, pkt_type) + data
        packet = struct.pack("<i", len(packet)) + packet
        self.sock.sendall(packet)

    def _recv(self):
        raw_len = self._recv_exact(4)
        (length,) = struct.unpack("<i", raw_len)
        payload = self._recv_exact(length)
        req_id, resp_type = struct.unpack("<ii", payload[:8])
        body = payload[8:-2].decode("utf-8", errors="replace")
        return req_id, resp_type, body

    def _recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RCONError("connection closed by server")
            buf += chunk
        return buf

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()
