"""Shot / scene specification — JSON-serializable."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class ShotSpec:
    scene: str
    outfile: str
    title: str | None = None
    core_idea: str | None = None
    params: dict[str, Any] = field(default_factory=dict)
    seed: int = 7
    ss: int = 3
    character: str = "xiaohuang"  # xiaohuang | xiaohei

    def merged_params(self) -> dict[str, Any]:
        p = dict(self.params)
        if self.title is not None:
            p.setdefault("title", self.title)
        return p

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> ShotSpec:
        return cls(
            scene=d["scene"],
            outfile=d["outfile"],
            title=d.get("title"),
            core_idea=d.get("core_idea"),
            params=dict(d.get("params") or {}),
            seed=int(d.get("seed", 7)),
            ss=int(d.get("ss", 3)),
            character=str(d.get("character", "xiaohuang")),
        )

    @classmethod
    def load(cls, path: str | Path) -> ShotSpec | list[ShotSpec]:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if isinstance(data, list):
            return [cls.from_dict(x) for x in data]
        return cls.from_dict(data)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)
