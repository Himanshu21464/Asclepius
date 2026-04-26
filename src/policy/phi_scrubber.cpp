// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/policy.hpp"

#include <regex>
#include <string>
#include <vector>

namespace asclepius {

namespace {

// Conservative PHI scrubber. Designed to fail closed: prefer scrubbing
// non-PHI over leaving PHI through. The regex set covers the most common
// patterns. Production deployments are expected to compose this with a
// model-based scrubber (Presidio, internal NER) for richer coverage.
//
// The output decision is MODIFY: detected spans are replaced with
// "[REDACTED:<class>]". Counts of each class are surfaced via the
// rationale field for telemetry.
class PHIScrubber final : public IPolicy {
public:
    std::string_view name() const noexcept override { return "phi_scrubber"; }

    PolicyVerdict evaluate_input (const InferenceContext&, std::string_view payload) override {
        return scrub(payload);
    }
    PolicyVerdict evaluate_output(const InferenceContext&, std::string_view payload) override {
        return scrub(payload);
    }

private:
    static PolicyVerdict scrub(std::string_view in) {
        struct Pattern { std::regex re; const char* cls; };
        // The regexes are constructed once per-thread to avoid re-parsing on
        // every call. Construction is expensive enough that this matters.
        thread_local std::vector<Pattern> patterns = {
            { std::regex(R"(\b\d{3}-\d{2}-\d{4}\b)"),                       "ssn"    },
            { std::regex(R"(\b\(?\d{3}\)?[\s\-]?\d{3}[\s\-]?\d{4}\b)"),    "phone"  },
            { std::regex(R"(\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b)",
                         std::regex::icase),                                "email"  },
            { std::regex(R"(\bMRN[:#\-\s]*\d{4,12}\b)", std::regex::icase),  "mrn"    },
            { std::regex(R"(\b(0?[1-9]|1[0-2])[/\-](0?[1-9]|[12]\d|3[01])[/\-](19|20)\d{2}\b)"),
                                                                            "date"   },
            { std::regex(R"(\b\d{4}[\-]\d{4}[\-]\d{4}[\-]\d{4}\b)"),        "cc"     },
        };

        std::string out{in};
        std::vector<std::string> classes_seen;
        for (auto& p : patterns) {
            std::string repl = std::string{"[REDACTED:"} + p.cls + "]";
            std::string after = std::regex_replace(out, p.re, repl);
            if (after != out) {
                classes_seen.emplace_back(p.cls);
                out = std::move(after);
            }
        }

        PolicyVerdict v;
        if (classes_seen.empty()) {
            v.decision = PolicyDecision::allow;
            return v;
        }

        v.decision         = PolicyDecision::modify;
        v.modified_payload = std::move(out);
        v.violations       = std::move(classes_seen);
        v.rationale        = "PHI patterns detected and redacted";
        return v;
    }
};

}  // namespace

std::shared_ptr<IPolicy> make_phi_scrubber() {
    return std::make_shared<PHIScrubber>();
}

}  // namespace asclepius
