"""Full zero-delay Morrison model and compact ActiveN ABI (not sparse Brunel).

The reference is stdp-experiments/workloads/morrison.py. Runtime arithmetic and
event order retain the ActiveN Brunel approximations approved for this port.
"""
from dataclasses import asdict, dataclass
import math
import numpy as np

F = np.float32
MAGIC = 0x4D4F5231
ABI_VERSION = 1
NODE_BASE, NODE_BYTES, MAX_NEURONS = 0x100, 32, 220
FIRED_LIST, CONFIG, JOBS, SEEN, DESCRIPTOR = 0x1C80, 0x2000, 0x2040, 0x2720, 0x27A0
PARAMETERS, POISSON, POISSON_COUNT = 0x2E00, 0x2E40, 20
INIT_END, SPM_BYTES, LINE, CHUNK = 0x3F00, 0x4000, 32, 128
MC_BYTES = 1 << 30
FAMILY = "morrison-2007-zero-delay-v1"
GROUPS = ("ee", "ei", "ie", "ii")


@dataclass(frozen=True)
class Spec:
    ne: int = 90000
    ni: int = 22500
    ce: int = 9000
    ci: int = 2250
    pu: int = 512
    mc: int = 16

    @property
    def n(self):
        return self.ne + self.ni

    @property
    def pu_per_mc(self):
        return self.pu // self.mc

    def validate(self, functional=False):
        if not functional and self != Spec():
            raise ValueError("non-target dimensions require an explicit functional test")
        if (min(self.ne, self.ni) < 2 or min(self.ce, self.ci) < 1
                or self.pu not in (64, 512) or self.mc not in (2, 16)
                or self.pu % self.mc or self.pu_per_mc < 16
                or math.ceil(self.n / self.pu) > MAX_NEURONS):
            raise ValueError("invalid compact model/topology or neuron capacity")
        if self.n * (self.ce + self.ci) >= 2**32:
            raise ValueError("forward per-step reduction would overflow uint32")

    def projections(self):
        return {
            "ee": (self.ne, self.ne, self.ce, 0, 0),
            "ei": (self.ne, self.ni, self.ce, 0, self.ne),
            "ie": (self.ni, self.ne, self.ci, self.ne, 0),
            "ii": (self.ni, self.ni, self.ci, self.ne, self.ne),
        }

    def model(self):
        return dict(family=FAMILY, ne=self.ne, ni=self.ni, ce=self.ce, ci=self.ci,
                    dt_ms=.1, delay_ms=0., tau_syn_ms=.33, tau_m_ms=10.,
                    capacitance_pf=250., refractory_ms=.5, v_threshold_mv=20.,
                    v_reset_mv=0., v_rest_mv=0., je_pa=45.61,
                    tau_plus_ms=20., tau_minus_ms=20., external_rate_hz=20880.,
                    recurrent_delivery_scale=1., external_rate_scale=1.,
                    rule=dict(name="morrison", inhibitory_weight_ratio=5.,
                              external_drive_eta=1., learning_rate=.1,
                              depression_ratio=.1057, mu_plus=.4, mu_minus=1.,
                              weight_max_pa=None))

    def system(self):
        return dict(pus=self.pu, mcs=self.mc, mc_bytes=MC_BYTES,
                    physical_bytes=self.mc * MC_BYTES, memory_line_bytes=LINE,
                    spm_bytes=SPM_BYTES, smt_pipes=2, icache_bytes=8192)


def constants():
    dt, tau, tm, cm = .1, .33, 10., 250.
    beta = tau * tm / (tm - tau)
    decay = math.exp(-dt / tau)
    ex = math.expm1(dt * (tm - tau) / (tau * tm))
    scale = math.e / tau
    return {k: F(v) for k, v in dict(
        p11=decay, p21=dt * decay,
        p31=beta / cm * decay * (beta * ex - dt),
        p32=beta / cm * decay * ex, p33=math.exp(-dt / tm),
        epsc=scale, delivery=1., external_mean=2.088,
        external_limit=math.exp(-2.088), external_weight=45.61,
        dep=.1 * .1057, pot=.1 * scale**.6, wmax=0.,
        decay_pre=math.exp(-dt / 20.), decay_post=math.exp(-dt / 20.),
        inhibitory_weight=-5. * 45.61).items()}


def poisson_thresholds(mean=2.088):
    """Inclusive quantiles for a nonzero xorshift32 draw, with no cap at nine.

    The final saturated cutoff covers every possible draw. This is a discrete
    inverse CDF at 32-bit uniform resolution, not an infinite-precision Poisson.
    """
    if not math.isfinite(mean) or not 0 <= mean <= 2.088001:
        raise ValueError("sampler table supports Morrison's mean or a functional zero drive")
    probability, cumulative = math.exp(-mean), 0.
    thresholds = []
    for k in range(POISSON_COUNT):
        if k:
            probability *= mean / k
        cumulative += probability
        thresholds.append(min(2**32 - 1, int(cumulative * (2**32 - 1))))
    # Sub-ULP tails cannot be represented by this RNG; the largest draw must
    # terminate inside the table even if floating summation undershoots 1.
    thresholds[-1] = 2**32 - 1
    return np.asarray(thresholds, dtype="<u4")


def cold_state(spec, seed):
    rng = np.random.default_rng(seed)
    state = np.zeros((spec.n, 8), dtype="<u4")
    state[:, 0] = rng.normal(5.7, 7.2, spec.n).astype(F).view(np.uint32)
    return state


def hot_state(data, spec, traces):
    """Convert split GeNN filters once; scale the two cached E coefficients."""
    state = np.zeros((spec.n, 8), dtype="<u4")
    f, c = state.view(F), constants()
    for name, start, count in (("exc", 0, spec.ne), ("inh", spec.ne, spec.ni)):
        fields = {}
        for key in ("V", "Iex", "Iin", "dIex", "dIin", "refrac"):
            a = np.asarray(data[f"{name}_{key}"]).reshape(-1)
            if a.shape != (count,) or not np.all(np.isfinite(a)):
                raise ValueError(f"invalid {name}_{key}")
            fields[key] = a
        refrac = fields["refrac"]
        if np.any(refrac < 0) or np.any(refrac > 5) or np.any(refrac != refrac.astype(np.uint32)):
            raise ValueError("invalid refractory state")
        f[start:start+count, 0] = fields["V"]
        f[start:start+count, 1] = F(F(fields["Iex"]) + F(fields["Iin"]))
        f[start:start+count, 2] = F(F(fields["dIex"]) + F(fields["dIin"]))
        state[start:start+count, 7] = refrac.astype(np.uint32)
    f[:spec.ne, 3] = F(traces["pre"] * c["pot"])
    f[:spec.ne, 4] = F(traces["post"] * c["dep"])
    for group, start, count, col in (("ee", 0, spec.ne, 5), ("ie", 0, spec.ne, 6),
                                   ("ei", spec.ne, spec.ni, 5), ("ii", spec.ne, spec.ni, 6)):
        values = np.asarray(data[f"{group}_input"], F).reshape(-1)
        if values.shape != (count,) or not np.all(np.isfinite(values)):
            raise ValueError(f"invalid {group} pending input")
        f[start:start+count, col] = values
    return state
