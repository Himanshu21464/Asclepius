// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Asclepius-Med benchmark adapter scaffold.
// See README.md for the L2-Medical conformance mapping.
#pragma once

#include "asclepius/asclepius.hpp"

#include <functional>
#include <string>
#include <vector>

namespace asclepius::bench::asclepius_med {

struct BenchItem {
    std::string id;
    std::string specialty;
    std::string category;
    std::string sub_task;
    std::string prompt;
};

// Load the offline fixture (or any JSON shaped like fixture.json) into
// a vector of items. Returns Error on parse / shape failure.
asclepius::Result<std::vector<BenchItem>>
load_fixture(const std::string& path);

// Drive a single item through the wrapped runtime. The model callback
// is opaque to the substrate; the wrap is identical to examples/05 + 06.
//
// Each successful call appends exactly one `inference.committed` entry
// to the runtime's ledger, body-tagged with the item's specialty and
// sub_task so downstream regulators can subset by specialty without
// rerunning the model.
//
// Returns the model's reply text on success.
asclepius::Result<std::string>
drive_one(asclepius::Runtime& runtime,
          const BenchItem& item,
          const asclepius::ConsentToken& token,
          asclepius::PatientId patient,
          asclepius::ActorId actor,
          asclepius::TenantId tenant,
          asclepius::ModelId model,
          std::function<asclepius::Result<std::string>(std::string)> call_model);

}  // namespace asclepius::bench::asclepius_med
