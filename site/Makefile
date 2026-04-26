# Asclepius — site Makefile.
#
# Coordinates the static-site regen tasks. Idempotent; safe to run repeatedly.
# Default target prints the menu.

PY      ?= python3
PORT    ?= 8765
ROOT    := $(shell pwd)

NODE    ?= node

.PHONY: help validate integrity orphans content test manifest search-index status docs all ci serve clean

help:
	@echo "asclepius — site targets"
	@echo "  make validate       — HTML validator (broken links, dup ids, …)"
	@echo "  make integrity      — cross-reference audit (sw, manifest, sitemap, palette)"
	@echo "  make orphans        — find files not reachable by any link from /"
	@echo "  make content        — page-depth audit (word counts)"
	@echo "  make test           — run JS test suite (tests/run.mjs · ~110 tests)"
	@echo "  make manifest       — regen asset-manifest.json (SHA-256 of every served file)"
	@echo "  make search-index   — regen search-index.json (one record per page)"
	@echo "  make status         — regen status.json (semantic liveness + counts)"
	@echo "  make docs           — render docs/*.md → architecture/spec/threat-model.html"
	@echo "  make all            — regen + validate + every audit gate"
	@echo "  make ci             — make all + make test (suitable for CI)"
	@echo "  make serve          — local http server on :$(PORT)"
	@echo "  make clean          — remove .pyc and __pycache__"

validate:
	@$(PY) check_html.py

integrity:
	@$(PY) check_integrity.py

orphans:
	@$(PY) check_orphans.py

content:
	@$(PY) check_content.py

test:
	@$(NODE) tests/run.mjs

manifest:
	@$(PY) regen_manifest.py

search-index:
	@$(PY) regen_search.py

status:
	@$(PY) regen_status.py

docs:
	@$(PY) build_docs.py

# Order: search-index → status (uses pages count) → manifest (SHAs everything
# downstream of those two) → validate → integrity (audits everything fresh).
all: search-index status manifest validate integrity orphans
	@echo "✓ site refreshed"

# CI-equivalent — all the gates plus the JS test suite.
ci: all test
	@echo "✓ ready to ship"

serve:
	@echo "serving http://localhost:$(PORT)"
	@$(PY) -m http.server $(PORT)

clean:
	@find . -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
	@find . -name '*.pyc' -delete 2>/dev/null || true
	@echo "cleaned"
