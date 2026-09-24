#!/usr/bin/env python3
"""Small end-to-end replay/authentication test; requires root and cryptography.
Run with a fresh SoC app so the inbound SA replay window starts empty.
"""
import argparse
import select
import socket
import struct
import time
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

a = argparse.ArgumentParser()
a.add_argument("--interface", default="ens1f0np0", help="host receive interface")
a.add_argument("--tx-interface", help="injection interface (default: receive interface)")
a.add_argument("--spi", type=lambda s: int(s, 0), default=0x2001)
a.add_argument("--host-mac", default="58:a2:e1:53:19:d6")
args = a.parse_args()
mac = bytes.fromhex(args.host_mac.replace(":", ""))
key = bytes.fromhex("aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899")
salt = bytes.fromhex("22334455")
prefix = b"DECAP-REPLAY-CHECK:"
def ip_header(length, proto, src, dst):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, length, 0, 0, 64, proto, 0,
                    socket.inet_aton(src), socket.inet_aton(dst))
    total = sum(struct.unpack("!10H", h))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return h[:10] + struct.pack("!H", (~total) & 65535) + h[12:]
def frame(seq, bad=False):
    payload = prefix + struct.pack("!I", seq) + bytes(range(64))
    udp = struct.pack("!HHHH", 12345, 3282, 8 + len(payload), 0) + payload
    inner = ip_header(20 + len(udp), 17, "172.16.1.129", "172.16.1.128") + udp
    pad = (-len(inner) - 2) % 4
    plain = inner + bytes(range(1, pad + 1)) + bytes([pad, 4])
    aad = struct.pack("!II", args.spi, seq)
    iv = struct.pack("!Q", seq)
    cipher = AESGCM(key).encrypt(salt + iv, plain, aad)
    if bad:
        cipher = cipher[:-1] + bytes([cipher[-1] ^ 1])
    esp = aad + iv + cipher
    outer = ip_header(20 + len(esp), 50, "172.16.1.128", "172.16.1.129")
    return b"\xff" * 6 + mac + b"\x08\x00" + outer + esp, inner

rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
rx.bind((args.interface, 0))
tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
tx.bind((args.tx_interface or args.interface, 0))
expected = {}
for seq, bad in [(1,False),(1,False),(4,False),(3,False),(3,False),
                 (300,False),(2,False),(301,True),(301,False),(0,False)]:
    packet, inner = frame(seq, bad)
    if seq in (1,4,3,300,301) and not bad:
        expected[seq] = inner
    tx.send(packet)
    time.sleep(0.02)
seen = []
deadline = time.monotonic() + 3
while time.monotonic() < deadline:
    if not select.select([rx], [], [], max(0, deadline - time.monotonic()))[0]:
        break
    packet, address = rx.recvfrom(65535)
    if address[2] == socket.PACKET_OUTGOING or len(packet) < 42:
        continue
    if packet[12:14] != b"\x08\x00" or packet[23] != 17:
        continue
    if packet[42:42 + len(prefix)] != prefix:
        continue
    seq = struct.unpack("!I", packet[42 + len(prefix):46 + len(prefix)])[0]
    length = struct.unpack("!H", packet[16:18])[0]
    assert packet[:6] == mac, "wrong destination MAC"
    assert packet[14:14 + length] == expected.get(seq), "plaintext differs from input"
    seen.append(seq)
print("received sequences:", seen)
assert sorted(seen) == sorted(expected), "missing, duplicate, or unexpected plaintext"
print("PASS: plaintext equality, duplicate/old/zero rejection, out-of-order acceptance, bad-tag isolation")
