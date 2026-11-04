#!/usr/bin/env python3
"""
Probe the USBIP server of the wireless ESP32-S3 DAP without installing the
usbip-win driver.

It performs an OP_REQ_DEVLIST (0x8005) handshake and prints the emulated
USB device descriptor, which is enough to prove that:

  PC --Wi-Fi--> ESP32-S3 -> TCP:3240 -> usbip_server -> CMSIS-DAP descriptor

Usage:
    1. Connect the PC to the DAP SoftAP (SSID "DAP", password "12345678")
    2. python tools/probe_usbip.py [host]        (default host: 192.168.4.1)

Expected output:  idVendor=0x.... idProduct=0x.... busid=1-1  (plus the
CMSIS-DAP interface class 0x03 HID or 0xFF vendor specific, depending on
USE_WINUSB).
"""
import socket
import struct
import sys

OP_REQ_DEVLIST = 0x8005
USBIP_VERSION = 0x0111

# struct usbip_usb_device (312 bytes, packed)
DEV_FMT = "!256s32sIIIHHHBBBBBB"
DEV_SIZE = struct.calcsize(DEV_FMT)
assert DEV_SIZE == 312, DEV_SIZE


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 3240

    print(f"[*] connecting to {host}:{port} ...")
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(struct.pack("!HHI", USBIP_VERSION, OP_REQ_DEVLIST, 0))

        header = recv_exact(s, 8)
        version, code, status = struct.unpack("!HHI", header)
        print(f"[+] reply version=0x{version:04x} code=0x{code:04x} status={status}")

        if code != 0x0005:
            print("[!] not an OP_REP_DEVLIST answer")
            return 1

        (ndev,) = struct.unpack("!I", recv_exact(s, 4))
        print(f"[+] {ndev} device(s) exported")

        for i in range(ndev):
            raw = recv_exact(s, DEV_SIZE)
            (path, busid, busnum, devnum, speed, vid, pid, bcd, cls, sub,
             proto, cfg, ncfg, nif) = struct.unpack(DEV_FMT, raw)

            print(f"    device {i}: busid={_cstr(busid)} "
                  f"idVendor=0x{vid:04x} idProduct=0x{pid:04x} bcdDevice=0x{bcd:04x}")
            print(f"              class=0x{cls:02x} subclass=0x{sub:02x} protocol=0x{proto:02x} "
                  f"interfaces={nif} speed={speed}")

            for j in range(nif):
                icls, isub, iproto, _pad = struct.unpack("!BBBB", recv_exact(s, 4))
                print(f"              interface {j}: class=0x{icls:02x} "
                      f"subclass=0x{isub:02x} protocol=0x{iproto:02x}")

    print("[+] USBIP handshake OK")
    return 0


def _cstr(raw: bytes) -> str:
    """USBIP 的字符串字段是定长且不保证有结束符，直接 decode 会炸。"""
    return raw.split(b"\x00")[0].decode("ascii", errors="replace").strip()


def recv_exact(sock: socket.socket, size: int) -> bytes:
    buf = b""
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("connection closed by device")
        buf += chunk
    return buf


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001 - simple CLI tool
        print(f"[!] failed: {exc}")
        sys.exit(1)
