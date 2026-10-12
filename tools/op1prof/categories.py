"""Host-path buckets for perf samples (profiling/categories.json)."""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

DEFAULT = Path(__file__).resolve().parents[2] / "profiling" / "categories.json"
UNCATEGORIZED = "uncategorized"


@dataclass(frozen=True)
class Categories:
    roots: Tuple[str, ...]
    rules: Tuple[Tuple[str, Tuple[str, ...]], ...]   # (bucket, patterns), in file order

    @staticmethod
    def load(path: Optional[Path] = None) -> "Categories":
        config = json.loads((path or DEFAULT).read_text())
        rules = tuple((entry["name"], tuple(entry["match"])) for entry in config["categories"])
        names = [name for name, _ in rules]
        if len(set(names)) != len(names) or UNCATEGORIZED in names:
            raise ValueError(f"{path or DEFAULT}: bucket names must be unique and not {UNCATEGORIZED!r}")
        return Categories(roots=tuple(config["roots"]), rules=rules)

    def complete(self, symbols: Sequence[str]) -> bool:
        """The call chain reaches the CPU loop. A chain broken by code
        without frame pointers stops early and cannot be attributed to its
        callers, so inclusive numbers only count complete chains."""
        return any(root in symbol for symbol in symbols for root in self.roots)

    def bucket(self, symbols: Sequence[str]) -> str:
        """The bucket of the frame nearest the leaf that matches a rule: a
        device read called from generated code is device time, and a
        library leaf takes the bucket of its caller."""
        for symbol in symbols:
            for name, patterns in self.rules:
                if any(pattern in symbol for pattern in patterns):
                    return name
        return UNCATEGORIZED

    def names(self) -> List[str]:
        return [name for name, _ in self.rules] + [UNCATEGORIZED]
