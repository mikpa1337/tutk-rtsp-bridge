"""
Port of IOTCPacket.hh/.cc: the 16-byte IOTC header, (de)serialization, and
the packet factory helpers (LAN search / NAT punch / knock payloads).
"""

from __future__ import annotations

import socket
import struct
from dataclasses import dataclass
from enum import IntEnum

IOTC_MAGIC_WORD = 0x0204
IOTC_DISCOVERY_PORT = 0x7FF9

# on-wire layout, all little-endian:
#   H magic, B version, B flags1, H length, H packetCount,
#   H type, H flags2, H rlySessionId, B chId, B reserved
_HEADER_FMT = "<HBBHHHHHBB"
HEADER_SIZE = struct.calcsize(_HEADER_FMT)
assert HEADER_SIZE == 16


class PacketType(IntEnum):
    HandshakeReq = 0x212
    HandshakeResp = 0x213
    RegisterNatType = 0x1005

    NatPunch = 0x301
    P2PKnock = 0x401
    P2PKnockResp = 0x402
    P2PKnockRespResp = 0x404

    DataSessionReq = 0x407
    DataSessionResp = 0x408
    CloseSessionReq1 = 0x417
    CloseSessionReq2 = 0x418
    CloseSessionAck = 0x511
    KeepaliveReq = 0x427
    KeepaliveResp = 0x428

    LanSearchReq = 0x601
    LanSearchResp = 0x602

    QueryCapabilityReq = 0x802
    QueryCapabilityResp = 0x803


@dataclass
class IOTCHeader:
    magic: int = IOTC_MAGIC_WORD
    version: int = 0
    flags1: int = 0
    length: int = 0
    packet_count: int = 0
    type: int = 0  # PacketType
    flags2: int = 0
    rly_session_id: int = 0
    ch_id: int = 0
    reserved: int = 0

    def pack(self) -> bytes:
        return struct.pack(
            _HEADER_FMT,
            self.magic, self.version, self.flags1, self.length,
            self.packet_count, int(self.type), self.flags2,
            self.rly_session_id, self.ch_id, self.reserved,
        )

    @classmethod
    def unpack(cls, buf: bytes) -> "IOTCHeader":
        (magic, version, flags1, length, packet_count, ptype,
         flags2, rly_session_id, ch_id, reserved) = struct.unpack(_HEADER_FMT, buf[:HEADER_SIZE])
        return cls(magic, version, flags1, length, packet_count, ptype,
                    flags2, rly_session_id, ch_id, reserved)


@dataclass
class IOTCPacket:
    header: IOTCHeader
    payload: bytes = b""

    def serialize(self) -> bytes:
        h = self.header
        h.length = len(self.payload)
        return h.pack() + self.payload

    @classmethod
    def deserialize(cls, buf: bytes) -> "IOTCPacket | None":
        if len(buf) < HEADER_SIZE:
            return None
        header = IOTCHeader.unpack(buf)
        if header.magic != IOTC_MAGIC_WORD:
            return None
        if len(buf) < HEADER_SIZE + header.length:
            return None
        payload = buf[HEADER_SIZE:HEADER_SIZE + header.length]
        return cls(header, payload)


class IOTCPacketFactory:
    @staticmethod
    def create_lan_search(uid: str) -> bytes:
        data = bytearray(36)
        data[0:20] = uid.encode()[:20].ljust(20, b"\x00")
        return bytes(data)

    @staticmethod
    def create_nat_punch(uid: str, ip: str, port: int) -> bytes:
        data = bytearray(36)
        data[0:20] = uid.encode()[:20].ljust(20, b"\x00")
        data[20:22] = b"\x02\x00"  # AF_INET
        data[22:24] = struct.pack("<H", port)
        data[24:28] = socket.inet_aton(ip)  # network byte order, matches asio's to_bytes()
        return bytes(data)

    @staticmethod
    def create_knock(uid: str) -> bytes:
        data = bytearray(24)
        data[0:20] = uid.encode()[:20].ljust(20, b"\x00")
        data[20:24] = b"\xAA\xBB\xCC\xDD"
        return bytes(data)
