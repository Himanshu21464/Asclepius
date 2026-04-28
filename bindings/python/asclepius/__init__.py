# SPDX-License-Identifier: Apache-2.0
"""Asclepius — a trust substrate for clinical AI.

Pythonic facade over the C++ runtime. The hot path is C++; this binding
exists for the AI Governance Office, evaluation pipelines, and integration
glue where Python ergonomics matter more than nanoseconds.
"""

from ._asclepius import (
    __version__,
    ErrorCode,
    Actor,
    Patient,
    Encounter,
    Model,
    Tenant,
    Purpose,
    Policy,
    PolicyChain,
    DriftMonitor,
    DriftReport,
    DriftSeverity,
    MetricRegistry,
    ConsentRegistry,
    ConsentToken,
    Inference,
    Runtime,
    # India profile primitives (ADR-013 + ADR-014)
    ConsentArtefact,
    FamilyGraph,
    FamilyRelation,
    EmergencyOverride,
    EmergencyOverrideToken,
    to_abdm_json,
    from_abdm_json,
    artefact_from_token,
    token_from_artefact,
)
from . import policies

__all__ = [
    "__version__",
    "ErrorCode",
    "Actor",
    "Patient",
    "Encounter",
    "Model",
    "Tenant",
    "Purpose",
    "Policy",
    "PolicyChain",
    "DriftMonitor",
    "DriftReport",
    "DriftSeverity",
    "MetricRegistry",
    "ConsentRegistry",
    "ConsentToken",
    "Inference",
    "Runtime",
    # India profile (ADR-013 + ADR-014)
    "ConsentArtefact",
    "FamilyGraph",
    "FamilyRelation",
    "EmergencyOverride",
    "EmergencyOverrideToken",
    "to_abdm_json",
    "from_abdm_json",
    "artefact_from_token",
    "token_from_artefact",
    "policies",
]
