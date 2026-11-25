"""Engine anchors: named addresses with byte signatures and provenance.

Source priority: ``re-db/builds/<build>/anchors.yaml`` (owned by the RE
workstream) if present, otherwise ``sdk/data/seed/anchors.seed.json``.

``sign``    : generate a unique signature for every anchor from the user's exe.
``verify``  : re-scan every signature and check it resolves to the recorded
              address (exactly one match). Run this on every new game build.
              Anchors with a ``chain`` (xref-chain anchors, no unique signature of
              their own) are resolved from their start point and verified the same way.
``manifest``: emit the per-build address manifest the runtime loads
              (exe SHA-256 -> anchor RVAs), research/01 recommendation.
"""
from __future__ import annotations

import json
import os
import struct
from dataclasses import dataclass, field
from typing import Any

from .paths import repo_root, sdk_root
from .pattern import Pattern, apply_chain, make_signature, scan
from .pe import PEImage


@dataclass
class Anchor:
    name: str
    kind: str                     # function | code | data | vtable
    rva: int | None               # expected RVA in the reference build
    signature: str | None = None  # IDA-style; matches the function start or ref instruction
    ref_rva: int | None = None    # for data/vtable: instruction that references the target
    rip: dict | None = None       # {disp_at, insn_len} for data/vtable resolution
    offset: int = 0               # anchor = match + offset (function/code kinds)
    confidence: str = "low"
    provenance: str = ""
    extra: dict[str, Any] = field(default_factory=dict)
    chain: dict | None = None     # {from_anchor | from_signature, steps: [...]}, see pattern.apply_chain

    @property
    def sig_site_rva(self) -> int | None:
        return self.ref_rva if self.kind in ("data", "vtable") else self.rva


def _parse_addr(entry: dict, base: int) -> tuple[int | None, int | None]:
    def conv(key_va, key_rva):
        if entry.get(key_rva) is not None:
            return int(str(entry[key_rva]), 16)
        if entry.get(key_va) is not None:
            return int(str(entry[key_va]), 16) - base
        return None
    return conv("va", "rva"), conv("ref_va", "ref_rva")


def load_anchors(build: str = "1.0.2.1", image_base: int = 0x140000000, path: str | None = None) -> tuple[list[Anchor], str]:
    """Return (anchors, source_path)."""
    candidates = [path] if path else [
        os.path.join(repo_root(), "re-db", "builds", build, "anchors.yaml"),
        os.path.join(repo_root(), "re-db", "builds", build, "anchors.json"),
        os.path.join(sdk_root(), "data", "seed", "anchors.seed.json"),
    ]
    for p in candidates:
        if p and os.path.exists(p):
            with open(p, "r", encoding="utf-8") as fh:
                if p.endswith((".yaml", ".yml")):
                    try:
                        import yaml  # type: ignore
                    except ImportError as exc:  # pragma: no cover
                        raise RuntimeError(f"{p} needs PyYAML (pip install pyyaml)") from exc
                    doc = yaml.safe_load(fh)
                else:
                    doc = json.load(fh)
            entries = doc["anchors"] if isinstance(doc, dict) and "anchors" in doc else doc
            out = []
            for e in entries:
                rva, ref = _parse_addr(e, image_base)
                known = {"name", "kind", "va", "rva", "ref_va", "ref_rva", "rip", "signature", "offset", "confidence",
                         "provenance", "chain"}
                out.append(Anchor(
                    name=e["name"], kind=e.get("kind", "function"), rva=rva, signature=e.get("signature"),
                    ref_rva=ref, rip=e.get("rip"), offset=int(e.get("offset", 0)),
                    confidence=e.get("confidence", "low"), provenance=e.get("provenance", ""),
                    extra={k: v for k, v in e.items() if k not in known}, chain=e.get("chain"),
                ))
            return out, p
    raise FileNotFoundError("no anchors source found")


def resolve_chain(image: PEImage, chain: dict, by_name: dict[str, Anchor] | None = None) -> tuple[int | None, str]:
    """Resolve an xref-chain: unique start (a signed, non-chain anchor or an inline signature),
    then the steps. Returns (rva, status)."""
    if chain.get("from_anchor"):
        src = (by_name or {}).get(chain["from_anchor"])
        if src is None:
            return None, "chain-from-unknown"
        if src.chain or not src.signature:
            return None, "chain-from-not-signed"
        start, status = resolve(image, src)
        if status != "ok":
            return None, f"chain-from-{status}"
    elif chain.get("from_signature"):
        hits = scan(image, Pattern.parse(chain["from_signature"]), limit=2)
        if len(hits) != 1:
            return None, "chain-from-not-found" if not hits else "chain-from-ambiguous"
        start = hits[0]
    else:
        return None, "chain-no-start"
    return apply_chain(image, start, chain.get("steps", []))


def resolve(image: PEImage, anchor: Anchor, by_name: dict[str, Anchor] | None = None) -> tuple[int | None, str]:
    """Resolve an anchor in ``image`` via its signature or chain. Returns (rva, status)."""
    if anchor.chain:
        return resolve_chain(image, anchor.chain, by_name)
    if not anchor.signature:
        return None, "no-signature"
    hits = scan(image, Pattern.parse(anchor.signature), limit=2)
    if not hits:
        return None, "not-found"
    if len(hits) > 1:
        return None, "ambiguous"
    site = hits[0]
    if anchor.kind in ("data", "vtable"):
        if not anchor.rip:
            return None, "missing-rip-spec"
        disp_raw = image.read_rva(site + anchor.rip["disp_at"], 4)
        disp = struct.unpack("<i", disp_raw)[0]
        return site + anchor.rip["insn_len"] + disp, "ok"
    return site + anchor.offset, "ok"


def sign_all(image: PEImage, anchors: list[Anchor], force: bool = False) -> dict[str, str]:
    """Generate signatures for anchors lacking one. Returns name -> status."""
    report = {}
    for a in anchors:
        if a.chain:
            report[a.name] = "chain"
            continue
        if a.signature and not force:
            report[a.name] = "kept"
            continue
        site = a.sig_site_rva
        if site is None or image.rva_to_off(site) is None:
            report[a.name] = "no-site"
            continue
        pat = make_signature(image, site)
        if pat is None:
            report[a.name] = "not-unique"
            continue
        a.signature = str(pat)
        report[a.name] = f"signed ({sum(pat.mask)}/{len(pat.mask)} fixed bytes)"
    return report


def verify_all(image: PEImage, anchors: list[Anchor]) -> list[dict]:
    rows = []
    by_name = {a.name: a for a in anchors}
    for a in anchors:
        got, status = resolve(image, a, by_name)
        if status == "ok" and a.rva is not None and got != a.rva:
            status = "moved"
        rows.append({"name": a.name, "status": status, "expected_rva": a.rva, "resolved_rva": got, "confidence": a.confidence})
    return rows


def save_seed(anchors: list[Anchor], path: str, meta: dict, image_base: int = 0x140000000) -> None:
    out = []
    for a in anchors:
        e: dict[str, Any] = {"name": a.name}
        e.update({k: v for k, v in a.extra.items()})
        e["kind"] = a.kind
        if a.rva is not None:
            e["va"] = hex(image_base + a.rva)
        if a.ref_rva is not None:
            e["ref_va"] = hex(image_base + a.ref_rva)
        if a.rip:
            e["rip"] = a.rip
        if a.offset:
            e["offset"] = a.offset
        if a.signature:
            e["signature"] = a.signature
        if a.chain:
            e["chain"] = a.chain
        e["confidence"] = a.confidence
        e["provenance"] = a.provenance
        out.append(e)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump({"_meta": meta, "anchors": out}, fh, indent=2)
        fh.write("\n")


def build_manifest(image: PEImage, anchors: list[Anchor], game_build: str) -> dict:
    """Per-build address manifest consumed by the runtime (exe hash -> RVAs)."""
    resolved = {}
    for row in verify_all(image, anchors):
        if row["status"] == "ok":
            resolved[row["name"]] = hex(row["resolved_rva"])
    return {
        "game_build": game_build,
        "exe_sha256": image.sha256,
        "image_base": hex(image.image_base),
        "anchors": resolved,
    }
