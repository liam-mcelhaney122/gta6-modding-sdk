"""Locations inside the project checkout."""
import os

_HERE = os.path.dirname(os.path.abspath(__file__))


def sdk_root() -> str:
    """.../GTA VI Modding/sdk"""
    return os.path.normpath(os.path.join(_HERE, "..", ".."))


def repo_root() -> str:
    """.../GTA VI Modding (holds re-db/, findings/, research/)"""
    return os.path.normpath(os.path.join(sdk_root(), ".."))
