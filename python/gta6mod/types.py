"""Value types shared by the scripting API."""
from __future__ import annotations

from dataclasses import dataclass
import math

# Script handles are plain ints, as in every RAGE title.
Entity = int
Ped = int
Vehicle = int
Object = int
Player = int
Hash = int


@dataclass(frozen=True)
class Vector3:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

    def __add__(self, o: "Vector3") -> "Vector3":
        return Vector3(self.x + o.x, self.y + o.y, self.z + o.z)

    def __sub__(self, o: "Vector3") -> "Vector3":
        return Vector3(self.x - o.x, self.y - o.y, self.z - o.z)

    def __mul__(self, k: float) -> "Vector3":
        return Vector3(self.x * k, self.y * k, self.z * k)

    def length(self) -> float:
        return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z)

    def __iter__(self):
        yield self.x
        yield self.y
        yield self.z
