"""Minimal PCSX2 PINE client (TCP, default slot 28011). Usage as a module: Pine().read32(addr) / read_block(addr, n)."""
import socket, struct


class Pine:
    def __init__(self, port=28011, host="127.0.0.1"):
        self.s = socket.create_connection((host, port), timeout=5)

    def _call(self, payload: bytes) -> bytes:
        self.s.sendall(struct.pack("<I", len(payload) + 4) + payload)
        hdr = self._recv(4)
        size = struct.unpack("<I", hdr)[0]
        body = self._recv(size - 4)
        if body[0] != 0:
            raise RuntimeError("PINE error")
        return body[1:]

    def _recv(self, n):
        out = b""
        while len(out) < n:
            chunk = self.s.recv(n - len(out))
            if not chunk:
                raise ConnectionError("closed")
            out += chunk
        return out

    def read32(self, addr):
        return struct.unpack("<I", self._call(struct.pack("<BI", 2, addr)))[0]

    def read_block(self, addr, n):
        # batched read32 commands
        payload = b"".join(struct.pack("<BI", 2, a) for a in range(addr, addr + n, 4))
        self.s.sendall(struct.pack("<I", len(payload) + 4) + payload)
        hdr = self._recv(4)
        size = struct.unpack("<I", hdr)[0]
        body = self._recv(size - 4)
        if body[0] != 0:
            raise RuntimeError("PINE batch error")
        return body[1:]

    def status(self):
        return struct.unpack("<I", self._call(struct.pack("<B", 0x0F)))[0]


def _write32(self, addr, value):
    self._call(struct.pack("<BII", 6, addr, value))


Pine.write32 = _write32
