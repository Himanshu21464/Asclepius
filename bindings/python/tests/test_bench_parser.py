# SPDX-License-Identifier: Apache-2.0
"""Standalone tests for the pure-Python parts of asclepius.bench.

These tests run without the pybind11 C++ extension built. Run them with:

    python3 bindings/python/tests/test_bench_parser.py

For the runner (``drive_one``) you need the full extension; the C++
adapter at ``tests/benchmarks/asclepius_med/`` already covers that path
end-to-end.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

# Allow `python3 path/to/this` without installing the package.
HERE = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(HERE)
sys.path.insert(0, PKG_DIR)
# Import only the pure-python sub-module without pulling __init__.py
# (which depends on the C++ extension).
import importlib.util
_spec = importlib.util.spec_from_file_location(
    "asclepius_bench_pure",
    os.path.join(PKG_DIR, "asclepius", "bench.py"),
)
bench = importlib.util.module_from_spec(_spec)
sys.modules["asclepius_bench_pure"] = bench  # required by dataclass on 3.14+
_spec.loader.exec_module(bench)

REPO_ROOT = os.path.abspath(os.path.join(PKG_DIR, "..", ".."))
FIXTURE = os.path.join(
    REPO_ROOT, "tests", "benchmarks", "asclepius_med", "fixture.json")


class BenchItemTests(unittest.TestCase):

    def test_from_dict_round_trip(self):
        d = {"id": "x", "specialty": "cardiology", "category": "c",
             "sub_task": "s", "prompt": "p"}
        it = bench.BenchItem.from_dict(d)
        self.assertEqual(it.id, "x")
        self.assertEqual(it.specialty, "cardiology")
        self.assertEqual(it.category, "c")
        self.assertEqual(it.sub_task, "s")
        self.assertEqual(it.prompt, "p")

    def test_from_dict_rejects_missing_field(self):
        for missing in ("id", "specialty", "category", "sub_task", "prompt"):
            d = {"id": "x", "specialty": "c", "category": "c",
                 "sub_task": "s", "prompt": "p"}
            del d[missing]
            with self.assertRaises(ValueError):
                bench.BenchItem.from_dict(d)


class LoadFixtureTests(unittest.TestCase):

    def test_loads_real_offline_fixture(self):
        items = bench.load_fixture(FIXTURE)
        self.assertEqual(len(items), 5)
        # All five fixture items have non-empty fields and stable ids.
        ids = {i.id for i in items}
        self.assertEqual(ids, {f"fix-00{n}" for n in range(1, 6)})
        # Specialties cover at least three distinct fields.
        specialties = {i.specialty for i in items}
        self.assertGreaterEqual(len(specialties), 3)

    def test_missing_file_raises(self):
        with self.assertRaises(FileNotFoundError):
            bench.load_fixture("/tmp/this-does-not-exist-asclepius.json")

    def test_malformed_top_level_raises(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"oops": True}, f)
            path = f.name
        try:
            with self.assertRaises(ValueError):
                bench.load_fixture(path)
        finally:
            os.unlink(path)

    def test_malformed_items_raises(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"items": [{"id": "missing-other-fields"}]}, f)
            path = f.name
        try:
            with self.assertRaises(ValueError):
                bench.load_fixture(path)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
