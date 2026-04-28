# SPDX-License-Identifier: Apache-2.0
"""Asclepius-Med benchmark adapter — Python parity.

Mirrors the C++ adapter at ``tests/benchmarks/asclepius_med/`` so callers
can drive a wrapped clinical model through the L2-Medical conformance
suite from Python without leaving the pybind11 boundary. ``BenchItem``
and ``load_fixture()`` are pure Python; ``drive_one()`` requires the
``_asclepius`` extension (it calls ``Runtime.begin_inference`` and the
new ``Inference.add_metadata`` shim).

The substrate certifies the bundle, not the score. Each call to
``drive_one`` writes one signed ``inference.committed`` ledger entry
tagged with the benchmark item's specialty and sub-task so a regulator
can subset by specialty without rerunning the model.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import Callable, List, Optional


@dataclass(frozen=True)
class BenchItem:
    id: str
    specialty: str
    category: str
    sub_task: str
    prompt: str

    @classmethod
    def from_dict(cls, d: dict) -> "BenchItem":
        for k in ("id", "specialty", "category", "sub_task", "prompt"):
            if k not in d:
                raise ValueError(f"benchmark item missing required field: {k}")
        return cls(
            id=str(d["id"]),
            specialty=str(d["specialty"]),
            category=str(d["category"]),
            sub_task=str(d["sub_task"]),
            prompt=str(d["prompt"]),
        )


def load_fixture(path: str) -> List[BenchItem]:
    """Parse a fixture file shaped like
    ``tests/benchmarks/asclepius_med/fixture.json``.

    Raises ``FileNotFoundError`` if the path does not exist;
    ``ValueError`` on any structural problem with the JSON.
    """
    if not os.path.exists(path):
        raise FileNotFoundError(f"fixture not found: {path}")
    with open(path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    if not isinstance(doc, dict) or "items" not in doc or not isinstance(doc["items"], list):
        raise ValueError("fixture missing items[] array")
    return [BenchItem.from_dict(it) for it in doc["items"]]


# ── runner ───────────────────────────────────────────────────────────────
# `drive_one` calls into the pybind11 extension. We import lazily so that
# `load_fixture` and `BenchItem` work in environments where the C++
# extension hasn't been built (e.g. CI fixtures, schema validators).

def drive_one(
    runtime,                # asclepius.Runtime
    item: BenchItem,
    token,                  # asclepius.ConsentToken
    patient,                # asclepius.Patient
    actor,                  # asclepius.Actor
    tenant,                 # asclepius.Tenant
    model,                  # asclepius.Model
    call_model: Callable[[str], str],
    *,
    encounter=None,         # asclepius.Encounter, default = new
) -> str:
    """Drive a single benchmark item through the wrapped runtime.

    Returns the model's reply on success. Each successful call appends
    exactly one ``inference.committed`` entry to ``runtime.ledger``,
    body-tagged with ``benchmark``, ``bench_item_id``, ``specialty``,
    ``category``, and ``sub_task`` keys (under the reserved
    ``metadata`` envelope).
    """
    # Lazy import — keeps load_fixture usable without the C++ ext.
    from . import Encounter, Purpose

    inf = runtime.begin_inference(
        model=model,
        actor=actor,
        patient=patient,
        encounter=encounter if encounter is not None else Encounter.make(),
        purpose=Purpose.DIAGNOSTIC_SUGGESTION,
        tenant=tenant,
        consent_token_id=token.token_id,
    )

    inf.add_metadata("benchmark",     "asclepius-med")
    inf.add_metadata("bench_item_id", item.id)
    inf.add_metadata("specialty",     item.specialty)
    inf.add_metadata("category",      item.category)
    inf.add_metadata("sub_task",      item.sub_task)

    out = inf.run(item.prompt, call_model)
    inf.commit()
    return out


__all__ = ["BenchItem", "load_fixture", "drive_one"]
