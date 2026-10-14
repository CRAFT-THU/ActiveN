from __future__ import annotations

import hashlib
import json
import math
import platform
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np


DT_MS = 0.1
# Retained for historical non-GeNN ports and explicitly requested legacy cases.
DELAY_MS = 1.5
GENN_DEFAULT_INDEGREE_SCALE = 0.05
GENN_DEFAULT_DELAY_MS = 0.0
GENN_DEFAULT_RECURRENT_DELIVERY_SCALE = 1.0 / math.sqrt(
    GENN_DEFAULT_INDEGREE_SCALE
)
GENN_DEFAULT_EXTERNAL_RATE_SCALES = {
    "additive": 0.47,
    "morrison": 0.32,
}
STDP_TIMING_MODES = ("arrival", "nest_dendritic")
STDP_TIE_MODES = (
    "framework_pre_first",
    "nest_post_first",
    "nest_exclude_zero",
    "nest_causal_boundary",
)
TAU_M_MS = 10.0
TAU_SYN_MS = 0.32582722403722841
CAPACITANCE_PF = 250.0
REFRACTORY_MS = 0.5
V_REST_MV = 0.0
V_RESET_MV = 0.0
V_THRESHOLD_MV = 20.0
VM_MEAN_MV = 5.7
VM_STD_MV = 7.2


def stdp_post_path_delay_ms(mode: str, delay_ms: float = DELAY_MS) -> float:
    if mode == "arrival":
        return 0.0
    if mode == "nest_dendritic":
        # NEST samples postsynaptic history at t_pre_source - dendritic_delay,
        # while these ports process presynaptic learning at t_pre_source + delay.
        return 2.0 * delay_ms
    raise ValueError(f"unsupported STDP timing mode: {mode}")


JE_PA = 45.609600316540956
BASE_NE = 9000
BASE_NI = 2250
BASE_CE = 9000
BASE_CI = 2250
TAU_PLUS_MS = 20.0
TAU_MINUS_MS = 30.0


@dataclass(frozen=True)
class Rule:
    name: str
    inhibitory_weight_ratio: float
    external_drive_eta: float
    learning_rate: float
    depression_ratio: float
    mu_plus: float
    mu_minus: float
    weight_max_pa: float | None


RULES = {
    "additive": Rule(
        name="additive",
        inhibitory_weight_ratio=5.0,
        external_drive_eta=1.685,
        learning_rate=0.01,
        depression_ratio=1.05,
        mu_plus=0.0,
        mu_minus=0.0,
        weight_max_pa=2.0 * JE_PA,
    ),
    "morrison": Rule(
        name="morrison",
        inhibitory_weight_ratio=8.0,
        external_drive_eta=3.2,
        learning_rate=0.1,
        depression_ratio=0.1026,
        mu_plus=0.4,
        mu_minus=1.0,
        weight_max_pa=None,
    ),
}


@dataclass(frozen=True)
class Model:
    rule: Rule
    network_scale: float
    indegree_scale: float
    ne: int
    ni: int
    ce: int
    ci: int
    delay_ms: float
    recurrent_delivery_scale: float
    external_rate_scale: float

    @property
    def recurrent_synapses(self) -> int:
        return (self.ne + self.ni) * (self.ce + self.ci)

    @property
    def plastic_synapses(self) -> int:
        return self.ne * self.ce

    @property
    def external_rate_hz(self) -> float:
        nu_threshold = V_THRESHOLD_MV / (
            self.ce * TAU_M_MS / CAPACITANCE_PF * JE_PA * math.e * TAU_SYN_MS
        )
        return (
            nu_threshold
            * self.rule.external_drive_eta
            * self.ce
            * self.external_rate_scale
            * 1000.0
        )

    def as_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result.update(
            {
                "dt_ms": DT_MS,
                "tau_m_ms": TAU_M_MS,
                "tau_syn_ms": TAU_SYN_MS,
                "capacitance_pf": CAPACITANCE_PF,
                "refractory_ms": REFRACTORY_MS,
                "v_rest_mv": V_REST_MV,
                "v_reset_mv": V_RESET_MV,
                "v_threshold_mv": V_THRESHOLD_MV,
                "vm_mean_mv": VM_MEAN_MV,
                "vm_std_mv": VM_STD_MV,
                "je_pa": JE_PA,
                "tau_plus_ms": TAU_PLUS_MS,
                "tau_minus_ms": TAU_MINUS_MS,
                "external_rate_hz": self.external_rate_hz,
                "recurrent_synapses": self.recurrent_synapses,
                "plastic_synapses": self.plastic_synapses,
            }
        )
        return result


def variance_preserving_scales(indegree_scale: float) -> tuple[float, float]:
    if not math.isfinite(indegree_scale) or indegree_scale <= 0.0:
        raise ValueError("indegree scale must be finite and positive")
    root_scale = math.sqrt(indegree_scale)
    return 1.0 / root_scale, root_scale


def make_model(
    rule: str,
    network_scale: float,
    indegree_scale: float,
    *,
    delay_ms: float = DELAY_MS,
    recurrent_delivery_scale: float = 1.0,
    external_rate_scale: float = 1.0,
) -> Model:
    if rule not in RULES:
        raise ValueError(f"unknown rule {rule!r}")
    positive_values = {
        "network scale": network_scale,
        "indegree scale": indegree_scale,
        "recurrent delivery scale": recurrent_delivery_scale,
        "external rate scale": external_rate_scale,
    }
    for name, value in positive_values.items():
        if not math.isfinite(value) or value <= 0.0:
            raise ValueError(f"{name} must be finite and positive")
    if not math.isfinite(delay_ms) or delay_ms < 0.0:
        raise ValueError("delay must be finite and non-negative")
    delay_steps = delay_ms / DT_MS
    if not math.isclose(delay_steps, round(delay_steps), abs_tol=1e-9):
        raise ValueError("delay must be an integer number of timesteps")
    return Model(
        rule=RULES[rule],
        network_scale=network_scale,
        indegree_scale=indegree_scale,
        ne=max(2, round(BASE_NE * network_scale)),
        ni=max(2, round(BASE_NI * network_scale)),
        ce=max(1, round(BASE_CE * indegree_scale)),
        ci=max(1, round(BASE_CI * indegree_scale)),
        delay_ms=delay_ms,
        recurrent_delivery_scale=recurrent_delivery_scale,
        external_rate_scale=external_rate_scale,
    )


def make_genn_default_model(rule: str, network_scale: float = 1.0) -> Model:
    if rule not in RULES:
        raise ValueError(f"unknown rule {rule!r}")
    return make_model(
        rule,
        network_scale,
        GENN_DEFAULT_INDEGREE_SCALE,
        delay_ms=GENN_DEFAULT_DELAY_MS,
        recurrent_delivery_scale=GENN_DEFAULT_RECURRENT_DELIVERY_SCALE,
        external_rate_scale=GENN_DEFAULT_EXTERNAL_RATE_SCALES[rule],
    )


def alpha_propagator(tau_syn_ms: float = TAU_SYN_MS) -> dict[str, float]:
    h = DT_MS
    beta = tau_syn_ms * TAU_M_MS / (TAU_M_MS - tau_syn_ms)
    gamma = beta / CAPACITANCE_PF
    decay_syn = math.exp(-h / tau_syn_ms)
    expm1_tau = math.expm1(h * (TAU_M_MS - tau_syn_ms) / (tau_syn_ms * TAU_M_MS))
    p32 = gamma * decay_syn * expm1_tau
    p31 = gamma * decay_syn * (beta * expm1_tau - h)
    return {
        "p11": decay_syn,
        "p21": h * decay_syn,
        "p22": decay_syn,
        "p30": -TAU_M_MS / CAPACITANCE_PF * math.expm1(-h / TAU_M_MS),
        "p31": p31,
        "p32": p32,
        "p33": math.exp(-h / TAU_M_MS),
        "epsc_initial": math.e / tau_syn_ms,
    }


def alpha_lif_step(
    *,
    voltage_mv: float,
    current_pa: float,
    derivative_pa_per_ms: float,
    delivered_weight_pa: float = 0.0,
    refractory: bool = False,
) -> tuple[float, float, float]:
    p = alpha_propagator()
    if not refractory:
        voltage_mv = (
            p["p31"] * derivative_pa_per_ms
            + p["p32"] * current_pa
            + p["p33"] * voltage_mv
        )
    current_pa = p["p21"] * derivative_pa_per_ms + p["p22"] * current_pa
    derivative_pa_per_ms = (
        p["p11"] * derivative_pa_per_ms + p["epsc_initial"] * delivered_weight_pa
    )
    return voltage_mv, current_pa, derivative_pa_per_ms


@dataclass
class PairTraceState:
    weight_pa: float = JE_PA
    pre_trace: float = 0.0
    post_trace: float = 0.0
    time_ms: float = 0.0

    def _advance(self, time_ms: float) -> None:
        if time_ms < self.time_ms:
            raise ValueError("events must be time ordered")
        elapsed = time_ms - self.time_ms
        self.pre_trace *= math.exp(-elapsed / TAU_PLUS_MS)
        self.post_trace *= math.exp(-elapsed / TAU_MINUS_MS)
        self.time_ms = time_ms

    def on_pre(self, time_ms: float, rule: Rule) -> float:
        self._advance(time_ms)
        if rule.name == "additive":
            assert rule.weight_max_pa is not None
            self.weight_pa = max(
                0.0,
                self.weight_pa
                - rule.depression_ratio
                * rule.learning_rate
                * rule.weight_max_pa
                * self.post_trace,
            )
        else:
            self.weight_pa = max(
                0.0,
                self.weight_pa
                - rule.learning_rate
                * rule.depression_ratio
                * self.weight_pa
                * self.post_trace,
            )
        delivered = self.weight_pa
        self.pre_trace += 1.0
        return delivered

    def on_post(self, time_ms: float, rule: Rule) -> None:
        self._advance(time_ms)
        if rule.name == "additive":
            assert rule.weight_max_pa is not None
            self.weight_pa = min(
                rule.weight_max_pa,
                self.weight_pa
                + rule.learning_rate * rule.weight_max_pa * self.pre_trace,
            )
        else:
            self.weight_pa += (
                rule.learning_rate * self.weight_pa**rule.mu_plus * self.pre_trace
            )
        self.post_trace += 1.0


def histogram_mode_count(histogram: np.ndarray) -> int:
    if histogram.size < 3 or not np.any(histogram):
        return 0
    smoothed = np.convolve(histogram.astype(np.float64), np.ones(3) / 3.0, mode="same")
    cutoff = 0.05 * float(smoothed.max())
    return int(
        np.count_nonzero(
            (smoothed[1:-1] > smoothed[:-2])
            & (smoothed[1:-1] >= smoothed[2:])
            & (smoothed[1:-1] >= cutoff)
        )
    )


def weight_stats(weights: np.ndarray, rule: Rule) -> dict[str, Any]:
    values = np.asarray(weights, dtype=np.float64).reshape(-1)
    if values.size == 0:
        raise ValueError("weight sample is empty")
    quantiles = np.quantile(values, [0.0, 0.01, 0.1, 0.5, 0.9, 0.99, 1.0])
    mean = float(np.mean(values))
    std = float(np.std(values))
    if rule.weight_max_pa is not None:
        upper = rule.weight_max_pa
        low = float(np.mean(values <= 0.1 * upper))
        high = float(np.mean(values >= 0.9 * upper))
    else:
        upper = max(float(np.quantile(values, 0.999)) * 1.05, mean * 2.0, 1.0)
        low = float(np.mean(values <= 0.1 * mean)) if mean > 0.0 else 1.0
        high = float(np.mean(values >= 1.9 * mean)) if mean > 0.0 else 0.0
    histogram, edges = np.histogram(values, bins=np.linspace(0.0, upper, 41))
    centered = values - mean
    skewness = float(np.mean(centered**3) / std**3) if std > max(abs(mean), 1.0) * 1e-12 else 0.0
    return {
        "rule": rule.name,
        "sample_size": int(values.size),
        "mean": mean,
        "std": std,
        "skewness": skewness,
        "minimum": float(quantiles[0]),
        "p01": float(quantiles[1]),
        "p10": float(quantiles[2]),
        "median": float(quantiles[3]),
        "p90": float(quantiles[4]),
        "p99": float(quantiles[5]),
        "maximum": float(quantiles[6]),
        "low_boundary_fraction": low,
        "high_boundary_fraction": high,
        "boundary_fraction": low + high,
        "histogram_mode_count": histogram_mode_count(histogram),
        "histogram_counts": histogram.tolist(),
        "histogram_edges": edges.tolist(),
    }


def spike_stats(
    times_ms: np.ndarray, senders: np.ndarray, duration_ms: float, n_record: int
) -> dict[str, float]:
    times = np.asarray(times_ms, dtype=np.float64)
    ids = np.asarray(senders)
    rate = float(times.size / (n_record * duration_ms) * 1000.0)
    if times.size == 0:
        return {"rate_hz": 0.0, "population_fano_3ms": 0.0, "mean_cv_isi": 0.0}
    bins = max(1, int(math.ceil(duration_ms / 3.0)))
    counts, _ = np.histogram(times, bins=bins, range=(0.0, duration_ms))
    fano = float(np.var(counts) / np.mean(counts)) if np.mean(counts) > 0.0 else 0.0
    cvs = []
    for sender in np.unique(ids):
        intervals = np.diff(times[ids == sender])
        if intervals.size >= 2 and np.mean(intervals) > 0.0:
            cvs.append(float(np.std(intervals) / np.mean(intervals)))
    return {
        "rate_hz": rate,
        "population_fano_3ms": fano,
        "mean_cv_isi": float(np.mean(cvs)) if cvs else 0.0,
    }


def create_output(path: Path, manifest: dict[str, Any]) -> None:
    path.mkdir(parents=True, exist_ok=False)
    with (path / "manifest.json").open("x", encoding="ascii") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")


def base_manifest(backend: str, model: Model) -> dict[str, Any]:
    return {
        "backend": backend,
        "command": sys.argv,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "host": platform.uname()._asdict(),
        "model": model.as_dict(),
        "python": platform.python_version(),
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
