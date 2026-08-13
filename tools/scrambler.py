"""
Port of Scrambler::TransCodePartial / ReverseTransCodePartial from
scrambler.cc/hh. Same fixed key, same permutation tables, same bit-level
rotations - byte for byte compatible with the C++ implementation on a
little-endian host (which is what the words-as-uint32 rotation assumes,
same as the original).

This is NOT encryption - it's a fixed, keyless-in-practice obfuscation
layer. See scrambler.hh's comments in the C++ tree for more on that.
"""

TUTK_XOR_KEY = b"Charlie is the designer of P2P!!"  # 32 bytes

PERM16 = [11, 9, 8, 15, 13, 10, 12, 14, 2, 1, 5, 0, 6, 4, 7, 3]
PERM8 = [7, 4, 3, 2, 1, 6, 5, 0]
PERM4 = [2, 3, 0, 1]
PERM2 = [1, 0]

_PERM_TABLES = {16: PERM16, 8: PERM8, 4: PERM4, 2: PERM2}


def _mix_bytes(data: bytes) -> bytes:
    """Permute a block. All four tables above are involutions (self-inverse),
    so the same function is used for both scrambling and descrambling."""
    perm = _PERM_TABLES.get(len(data))
    if perm is None:
        return bytes(data)  # unmatched size: passthrough, matches MixBytes' default case
    return bytes(data[p] for p in perm)


def _rotr32(x: int, n: int) -> int:
    n &= 31
    return ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF


def _rotl32(x: int, n: int) -> int:
    n &= 31
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def _rotate_words(data16: bytes, rotate, amounts) -> bytes:
    out = bytearray(16)
    for i in range(4):
        word = int.from_bytes(data16[4 * i:4 * i + 4], "little")
        word = rotate(word, amounts[i])
        out[4 * i:4 * i + 4] = word.to_bytes(4, "little")
    return bytes(out)


def _xor16(data16: bytes) -> bytes:
    key = TUTK_XOR_KEY[:16]
    return bytes(a ^ b for a, b in zip(data16, key))


def _scramble_block(src16: bytes) -> bytes:
    s = _rotate_words(src16, _rotr32, (1, 5, 9, 13))
    s = _xor16(s)
    s = _mix_bytes(s)
    return _rotate_words(s, _rotr32, (3, 7, 11, 15))


def _descramble_block(src16: bytes) -> bytes:
    s = _rotate_words(src16, _rotl32, (3, 7, 11, 15))
    s = _mix_bytes(s)
    s = _xor16(s)
    return _rotate_words(s, _rotl32, (1, 5, 9, 13))


def trans_code_partial(data: bytes, length: int | None = None) -> bytes:
    """Scramble `data` for sending on the wire. `length` mirrors the C++
    `len` parameter - only the first `length` bytes are transformed, the
    rest is passed through untouched. Defaults to the whole buffer."""
    if not data:
        return b""
    target = min(length, len(data)) if length is not None else len(data)
    out = bytearray(data)

    offset = 0
    while offset + 16 <= target:
        out[offset:offset + 16] = _scramble_block(bytes(data[offset:offset + 16]))
        offset += 16

    remain = target - offset
    if remain:
        key = TUTK_XOR_KEY[:remain]
        scratch = bytes(b ^ k for b, k in zip(data[offset:offset + remain], key))
        out[offset:offset + remain] = _mix_bytes(scratch)

    return bytes(out)


def reverse_trans_code_partial(data: bytes, length: int | None = None) -> bytes:
    """Descramble a received buffer. Mirrors trans_code_partial exactly."""
    if not data:
        return b""
    target = min(length, len(data)) if length is not None else len(data)
    out = bytearray(data)

    offset = 0
    while offset + 16 <= target:
        out[offset:offset + 16] = _descramble_block(bytes(data[offset:offset + 16]))
        offset += 16

    remain = target - offset
    if remain:
        mixed = _mix_bytes(data[offset:offset + remain])
        key = TUTK_XOR_KEY[:remain]
        out[offset:offset + remain] = bytes(b ^ k for b, k in zip(mixed, key))

    return bytes(out)


if __name__ == "__main__":
    # quick self-test: scramble/descramble should round-trip for any length
    import os
    for size in list(range(0, 40)) + [63, 64, 65, 128]:
        original = os.urandom(size)
        scrambled = trans_code_partial(original, size)
        recovered = reverse_trans_code_partial(scrambled, size)
        assert recovered == original, f"round-trip failed at size {size}"
    print("scrambler round-trip OK for all tested sizes")
