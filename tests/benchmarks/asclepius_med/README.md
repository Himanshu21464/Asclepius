# Asclepius-Med benchmark adapter

Scaffold for driving a wrapped clinical model through the
[Asclepius-Med][am] benchmark (ACL 2025), and certifying that the
exported bundle satisfies the four [L2-Medical][l2m] conformance
assertions.

[am]: https://github.com/Asclepius-Med/Asclepius
[l2m]: ../../../site/conformance.html#l2-medical

## What this is, and is not

* **Is.** A C++ runner that loads a JSON list of benchmark items
  (`{id, specialty, category, sub_task, prompt}`), drives each through a
  caller-supplied model callback wrapped by the substrate, and produces
  one signed `inference.committed` ledger entry per item plus a
  bundle-export. Plus four doctest assertions that re-verify the
  bundle.
* **Is not.** A redistribution of the Asclepius-Med dataset. The real
  dev split is fetched from the upstream repo by the operator; we ship
  a tiny offline `fixture.json` (5 items spanning 3 specialties) so the
  scaffold builds and tests without a network. License terms for the
  real benchmark are unconfirmed at time of writing — see
  [ADR-012](../../../site/decisions.html#adr-012).

## Layout

```
tests/benchmarks/asclepius_med/
├── adapter.hpp          BenchItem + BenchAdapter API
├── adapter.cpp          load_fixture() + drive_one()
├── fixture.json         5 offline items (3 specialties, 4 sub-tasks)
├── test_adapter.cpp     doctest cases for the four L2-Medical asserts
├── CMakeLists.txt
└── README.md            this file
```

## Build & run

```sh
cmake --build build -j --target asclepius_bench_asclepius_med
./build/tests/benchmarks/asclepius_med/asclepius_bench_asclepius_med
```

The runner is a doctest executable; pass `--list-test-cases` to enumerate.

## Mapping to the conformance row

Each test case asserts one of the four [L2-Medical][l2m] entries:

| ID | Test name | What it checks |
|---|---|---|
| `medical.bench.dev-set-coverage`     | `dev_set_coverage`     | exactly one `inference.committed` per fixture item |
| `medical.bench.purpose-bound`        | `purpose_bound`        | every entry's purpose is `diagnostic_suggestion` |
| `medical.bench.specialty-tagged`     | `specialty_tagged`     | every entry body contains the item's specialty + sub-task |
| `medical.bench.bundle-reverifies`    | `bundle_reverifies`    | exported bundle reverifies offline against the runtime's pubkey |

When the four pass against the real Asclepius-Med dev split, the
conformance row at `/conformance#l2-medical` flips from `0 / 4 proposed`
to `4 / 4 pass`. The substrate certifies the bundle, not the score.
