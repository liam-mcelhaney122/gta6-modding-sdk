"""RAGE string hashing (Jenkins one-at-a-time, "joaat").

``joaat`` matches GET_HASH_KEY: ASCII letters are lower-cased first.
``joaat_raw`` hashes bytes as given (case-sensitive variants, e.g. PSO field
names in newer RAGE titles per research/02).
"""

_MASK = 0xFFFFFFFF


def joaat_raw(data: bytes, seed: int = 0) -> int:
    h = seed & _MASK
    for c in data:
        h = (h + c) & _MASK
        h = (h + (h << 10)) & _MASK
        h ^= h >> 6
    h = (h + (h << 3)) & _MASK
    h ^= h >> 11
    h = (h + (h << 15)) & _MASK
    return h


def joaat(text: str) -> int:
    """GET_HASH_KEY-compatible hash of ``text`` (ASCII-lowercased, UTF-8 bytes)."""
    return joaat_raw(text.encode("utf-8").lower())


def joaat_signed(text: str) -> int:
    """Same hash as a signed 32-bit int (how scripts usually print it)."""
    h = joaat(text)
    return h - 0x100000000 if h & 0x80000000 else h
