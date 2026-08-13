#!/usr/bin/env python3
"""
Minimal IOTC transport-layer debug client.

Ports just the transport handshake out of IOTCCameraClient.cc's
session_state_machine (phases 1-3: LAN search, NAT punch, P2P knock) plus a
keepalive loop and a raw send/recv REPL - nothing AV/RTSP related, this is
for poking at the wire protocol interactively.

Mirrors the *main* client's header defaults (flags1=2, flags2=0x21,
version=6, rlySessionId=0 - the C++ session id field is never actually set
to anything else anywhere in that codebase), not junk.hpp's experimental
variant.

Usage:
    python3 iotc_client.py discover
    python3 iotc_client.py handshake <uid>
    python3 iotc_client.py repl <uid>
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from dataclasses import dataclass

from iotc_packet import (
    HEADER_SIZE,
    IOTC_DISCOVERY_PORT,
    IOTCHeader,
    IOTCPacket,
    IOTCPacketFactory,
    PacketType,
)
from scrambler import reverse_trans_code_partial, trans_code_partial


def hexdump(label: str, data: bytes) -> None:
    print(f"{label} ({len(data)} bytes):")
    print(" ".join(f"{b:02x}" for b in data))


@dataclass
class ReceivedPacket:
    packet: IOTCPacket
    addr: tuple


class IOTCDebugClient:
    def __init__(self, bind_ip: str = "0.0.0.0", bind_port: int = 0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((bind_ip, bind_port))
        self.camera_endpoint: tuple | None = None

    def set_broadcast(self, enabled: bool) -> None:
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, int(enabled))

    # -- low level -----------------------------------------------------

    def send(self, pkt_type: PacketType, payload: bytes = b"", target: tuple | None = None) -> None:
        if target is None:
            target = self.camera_endpoint
        if target is None:
            raise ValueError("no target address - either pass target= or set self.camera_endpoint first")

        header = IOTCHeader(type=int(pkt_type), flags1=2, flags2=0x21, version=6)

        # mirrors IOTCCameraClient::send_packet's switch statement
        if pkt_type in (PacketType.P2PKnock, PacketType.P2PKnockResp):
            header.flags2 = 0x233
            if pkt_type == PacketType.P2PKnock:
                header.reserved = 1
        elif pkt_type == PacketType.DataSessionReq:
            header.rly_session_id = 0x7195

        raw = IOTCPacket(header, payload).serialize()
        scrambled = trans_code_partial(raw, len(raw))
        self.sock.sendto(scrambled, target)

    def recv(self, timeout: float = 2.0) -> ReceivedPacket:
        self.sock.settimeout(timeout)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for a packet")
            self.sock.settimeout(remaining)
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                raise TimeoutError("timed out waiting for a packet")

            if len(data) < 16:
                continue  # too small to be real, same as the C++ client

            plain = reverse_trans_code_partial(data, len(data))
            pkt = IOTCPacket.deserialize(plain)
            if pkt is None:
                continue

            if self.camera_endpoint is None:
                self.camera_endpoint = addr
            return ReceivedPacket(pkt, addr)

    # -- phase 1: LAN search --------------------------------------------

    def discover(self, timeout: float = 2.0) -> list[tuple[str, tuple]]:
        """Broadcast a LAN search and collect every camera that answers
        within `timeout` (like junk.hpp's IotcLanProber - use this when you
        don't already know a uid)."""
        self.set_broadcast(True)
        payload = IOTCPacketFactory.create_lan_search("")
        self.send(PacketType.LanSearchReq, payload, target=("255.255.255.255", IOTC_DISCOVERY_PORT))

        found = []
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                self.sock.settimeout(remaining)
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                break
            if len(data) < 16:
                continue
            plain = reverse_trans_code_partial(data, len(data))
            pkt = IOTCPacket.deserialize(plain)
            if pkt is None or pkt.header.type != PacketType.LanSearchResp or len(pkt.payload) < 20:
                continue
            uid = pkt.payload[:20].split(b"\x00", 1)[0].decode(errors="replace")
            found.append((uid, addr))
            print(f"discovered {uid} at {addr}")
        return found

    def lan_search_for_uid(self, uid: str, timeout: float = 2.0) -> tuple[str, int]:
        """Directed search for a known uid - phase 1 of the real handshake.
        Returns the reflected (ip, port) the camera says we're coming from."""
        self.set_broadcast(True)
        payload = IOTCPacketFactory.create_lan_search(uid)
        self.send(PacketType.LanSearchReq, payload, target=("255.255.255.255", IOTC_DISCOVERY_PORT))

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            self.sock.settimeout(max(remaining, 0.01))
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                break
            if len(data) < 16:
                continue
            plain = reverse_trans_code_partial(data, len(data))
            pkt = IOTCPacket.deserialize(plain)
            if pkt is None or pkt.header.type != PacketType.LanSearchResp:
                continue
            if len(pkt.payload) < 28:
                raise RuntimeError("LAN search response too short")
            reflected_port = int.from_bytes(pkt.payload[22:24], "little")
            reflected_ip = socket.inet_ntoa(pkt.payload[24:28])
            return reflected_ip, reflected_port

        raise TimeoutError(f"no LanSearchResp for uid={uid!r}")

    # -- phases 2-3: NAT punch + P2P knock -------------------------------

    def handshake(self, uid: str, timeout: float = 2.0) -> None:
        """Phases 1-3 from session_state_machine: LAN search, NAT punch
        (fire and forget), P2P knock / knock-resp / knock-resp-resp. Leaves
        self.camera_endpoint set and ready for keepalive() or manual send()."""
        reflected_ip, reflected_port = self.lan_search_for_uid(uid, timeout)
        print(f"reflected endpoint: {reflected_ip}:{reflected_port}")

        self.set_broadcast(False)

        punch_payload = IOTCPacketFactory.create_nat_punch(uid, reflected_ip, reflected_port)
        self.send(PacketType.NatPunch, punch_payload)  # no response expected

        knock_payload = IOTCPacketFactory.create_knock(uid)
        self.send(PacketType.P2PKnock, knock_payload)
        self._wait_for(PacketType.P2PKnock, timeout)

        self.send(PacketType.P2PKnockResp, knock_payload)
        self._wait_for(PacketType.P2PKnockResp, timeout)
        self._wait_for(PacketType.P2PKnockRespResp, timeout)

        print(f"handshake complete, camera endpoint = {self.camera_endpoint}")

    def _wait_for(self, expected: PacketType, timeout: float) -> ReceivedPacket:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"timed out waiting for {expected.name}")
            rp = self.recv(remaining)
            if rp.packet.header.type == expected:
                return rp

    def keepalive_forever(self, interval: float = 3.0) -> None:
        while True:
            time.sleep(interval)
            self.send(PacketType.KeepaliveReq)
            print("sent KeepaliveReq")


# -------------------------------------------------------------------------
# CLI
# -------------------------------------------------------------------------

def cmd_discover(args: argparse.Namespace) -> None:
    client = IOTCDebugClient(bind_ip=args.bind_ip)
    cams = client.discover(timeout=args.timeout)
    if not cams:
        print("no cameras found")


def cmd_handshake(args: argparse.Namespace) -> None:
    client = IOTCDebugClient(bind_ip=args.bind_ip)
    client.handshake(args.uid, timeout=args.timeout)
    if args.keepalive:
        try:
            client.keepalive_forever()
        except KeyboardInterrupt:
            pass


def cmd_repl(args: argparse.Namespace) -> None:
    """Do the handshake, then drop into a loop where you can fire arbitrary
    packet types at the camera and see what comes back - for poking around
    without wiring up the full AV/RTSP path."""
    client = IOTCDebugClient(bind_ip=args.bind_ip)
    client.handshake(args.uid, timeout=args.timeout)

    print()
    print("Connected. Commands:")
    print("  send <TypeName> [hex payload]   e.g.: send KeepaliveReq")
    print("  recv [timeout_seconds]")
    print("  quit")
    print("Known types:", ", ".join(t.name for t in PacketType))
    print()

    while True:
        try:
            line = input("> ").strip()
        except EOFError:
            break
        if not line or line == "quit":
            break

        parts = line.split()
        cmd = parts[0]

        if cmd == "send":
            if len(parts) < 2:
                print("usage: send <TypeName> [hex payload]")
                continue
            try:
                pkt_type = PacketType[parts[1]]
            except KeyError:
                print(f"unknown packet type {parts[1]!r}")
                continue
            payload = bytes.fromhex(parts[2]) if len(parts) > 2 else b""
            client.send(pkt_type, payload)
            print(f"sent {pkt_type.name} ({len(payload)} byte payload)")

        elif cmd == "recv":
            timeout = float(parts[1]) if len(parts) > 1 else 2.0
            try:
                rp = client.recv(timeout)
            except TimeoutError:
                print("(timeout)")
                continue
            try:
                type_name = PacketType(rp.packet.header.type).name
            except ValueError:
                type_name = f"0x{rp.packet.header.type:x}"
            print(f"from {rp.addr}: type={type_name} flags1={rp.packet.header.flags1:#x} "
                  f"flags2={rp.packet.header.flags2:#x} rlySessionId={rp.packet.header.rly_session_id:#x}")
            hexdump("payload", rp.packet.payload)

        else:
            print(f"unknown command {cmd!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bind-ip", default="0.0.0.0", help="local interface to bind to (default: all interfaces)")
    sub = parser.add_subparsers(dest="command", required=True)

    p_discover = sub.add_parser("discover", help="broadcast and list every camera that answers")
    p_discover.add_argument("--timeout", type=float, default=2.0)
    p_discover.set_defaults(func=cmd_discover)

    p_handshake = sub.add_parser("handshake", help="run the LAN search / NAT punch / knock sequence for a known uid")
    p_handshake.add_argument("uid")
    p_handshake.add_argument("--timeout", type=float, default=2.0)
    p_handshake.add_argument("--keepalive", action="store_true", help="keep sending KeepaliveReq afterwards, forever")
    p_handshake.set_defaults(func=cmd_handshake)

    p_repl = sub.add_parser("repl", help="handshake, then send/recv raw packets interactively")
    p_repl.add_argument("uid")
    p_repl.add_argument("--timeout", type=float, default=2.0)
    p_repl.set_defaults(func=cmd_repl)

    args = parser.parse_args()
    try:
        args.func(args)
    except (TimeoutError, RuntimeError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
