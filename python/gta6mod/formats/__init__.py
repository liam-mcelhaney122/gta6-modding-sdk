"""Readers for GTA VI container formats (read-only, header level for now).

Field names follow what has been *observed* (findings/00_static_triage.md);
fields whose meaning is not yet established keep neutral names (``field_0c``)
and are documented as unknown. Encrypted parts (the RPF9 TOC) are exposed as
raw bytes; decryption is Phase-3 work (findings/01 when available).
"""
from .rpf9 import Rpf9Header, read_rpf9_header
from .rds5 import Rds5Header, read_rds5_header

__all__ = ["Rpf9Header", "read_rpf9_header", "Rds5Header", "read_rds5_header"]
