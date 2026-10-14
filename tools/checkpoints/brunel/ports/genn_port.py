from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path
from typing import Any

import numpy as np

from ports.common import (
    DT_MS,
    GENN_DEFAULT_DELAY_MS,
    GENN_DEFAULT_EXTERNAL_RATE_SCALES,
    GENN_DEFAULT_INDEGREE_SCALE,
    GENN_DEFAULT_RECURRENT_DELIVERY_SCALE,
    JE_PA,
    REFRACTORY_MS,
    TAU_MINUS_MS,
    TAU_PLUS_MS,
    TAU_SYN_MS,
    STDP_TIMING_MODES,
    STDP_TIE_MODES,
    VM_MEAN_MV,
    VM_STD_MV,
    V_RESET_MV,
    V_THRESHOLD_MV,
    Model,
    alpha_propagator,
    base_manifest,
    create_output,
    make_model,
    spike_stats,
    stdp_post_path_delay_ms,
    weight_stats,
)


# Zero marks an event that has never occurred. Emission ticks are one-based,
# so adding the configured delay gives GeNN's next-tick delivery index.
STDP_TICK_MAX = (1 << 32) - 1


def stdp_timestamp_metadata() -> dict[str, Any]:
    return {
        "stdp_timestamp_type": "uint32",
        "stdp_timestamp_unit_ms": DT_MS,
        "stdp_tie_comparison": "exact integer equality",
        "stdp_elapsed_time": "integer subtraction, then scalar conversion and multiplication by dt",
        "stdp_clock_guard": "kernel assertion before uint32 overflow",
    }


def _import_genn() -> dict[str, Any]:
    try:
        import pygenn
        from pygenn import (
            GeNNModel,
            ParallelismHint,
            create_neuron_model,
            create_sparse_connect_init_snippet,
            create_weight_update_model,
            init_postsynaptic,
            init_sparse_connectivity,
            init_weight_update,
        )
        from pygenn.genn_model import backend_modules
    except ImportError as error:
        raise SystemExit("PyGeNN is unavailable; use brunel/run_with_reimpl_env.sh") from error
    return {
        "module": pygenn,
        "GeNNModel": GeNNModel,
        "ParallelismHint": ParallelismHint,
        "create_neuron_model": create_neuron_model,
        "create_sparse_connect_init_snippet": create_sparse_connect_init_snippet,
        "create_weight_update_model": create_weight_update_model,
        "init_postsynaptic": init_postsynaptic,
        "init_sparse_connectivity": init_sparse_connectivity,
        "init_weight_update": init_weight_update,
        "backend_modules": backend_modules,
    }


class GeNNBrunel:
    def __init__(
        self,
        *,
        spec: Model,
        seed: int,
        state_seed: int | None,
        backend: str,
        build_path: Path,
        recording_steps: int,
        precision: str = "double",
        ee_parallelism: str = "postsynaptic",
        ee_num_threads_per_spike: int = 1,
        stdp_timing: str = "nest_dendritic",
        stdp_tie_order: str = "nest_causal_boundary",
        timing_enabled: bool = True,
        reuse_build: Path | None = None,
        record_spikes: bool = True,
        collect_connectivity_stats: bool = True,
        connectivity: dict[str, tuple[np.ndarray, np.ndarray]] | None = None,
    ) -> None:
        api = _import_genn()
        if backend not in api["backend_modules"]:
            available = ", ".join(api["backend_modules"])
            raise RuntimeError(f"GeNN backend {backend!r} unavailable; available: {available}")
        create_neuron_model = api["create_neuron_model"]
        create_weight_update_model = api["create_weight_update_model"]
        init_weight_update = api["init_weight_update"]
        init_postsynaptic = api["init_postsynaptic"]
        init_sparse_connectivity = api["init_sparse_connectivity"]
        delay_steps = round(spec.delay_ms / DT_MS)
        post_delay_steps = round(stdp_post_path_delay_ms(stdp_timing, spec.delay_ms) / DT_MS)
        if not 0 <= max(delay_steps, post_delay_steps) < STDP_TICK_MAX:
            raise ValueError("synaptic delays exceed the uint32 tick range")
        if min(delay_steps, post_delay_steps) < 0:
            raise ValueError("synaptic delays must be nonnegative")
        self.max_spike_tick = STDP_TICK_MAX - max(delay_steps, post_delay_steps)
        p = alpha_propagator(getattr(spec, "tau_syn_ms", TAU_SYN_MS))
        je_pa = getattr(spec, "je_pa", JE_PA)
        neuron_model = create_neuron_model(
            "BrunelAlphaLIF",
            params=[
                "p11",
                "p21",
                "p22",
                "p31",
                "p32",
                "p33",
                "epscInitial",
                "externalMean",
                "externalWeight",
                "threshold",
                "reset",
                "refractorySteps",
                ("maxSpikeTick", "uint32_t"),
            ],
            vars=[
                ("V", "scalar"),
                ("Iex", "scalar"),
                ("dIex", "scalar"),
                ("Iin", "scalar"),
                ("dIin", "scalar"),
                ("refrac", "unsigned int"),
                ("spikeCount", "unsigned int"),
                ("tick", "uint32_t"),
                ("lastSpikeTick", "uint32_t"),
            ],
            additional_input_vars=[("exIn", "scalar", 0.0), ("inIn", "scalar", 0.0)],
            sim_code="""
            assert(tick < maxSpikeTick);
            tick++;
            if (refrac == 0) {
                V = (p31 * dIex) + (p32 * Iex) + (p31 * dIin)
                    + (p32 * Iin) + (p33 * V);
            }
            else {
                refrac--;
            }
            Iex = (p21 * dIex) + (p22 * Iex);
            dIex *= p11;
            Iin = (p21 * dIin) + (p22 * Iin);
            dIin *= p11;
            dIex += exIn;
            dIin += inIn;
            unsigned int externalSpikes = 0;
            const scalar poissonLimit = exp(-externalMean);
            scalar poissonProduct = gennrand_uniform();
            while (poissonProduct > poissonLimit) {
                externalSpikes++;
                poissonProduct *= gennrand_uniform();
            }
            dIex += epscInitial * externalWeight * (scalar)externalSpikes;
            """,
            threshold_condition_code="(refrac == 0) && (V >= threshold)",
            reset_code="""
            V = reset;
            refrac = (unsigned int)refractorySteps;
            spikeCount++;
            lastSpikeTick = tick;
            """,
        )
        plastic_model = create_weight_update_model(
            "BrunelMeasuredSTDP",
            params=[
                "epscInitial",
                "deliveryScale",
                "learningRate",
                "depressionRatio",
                "muPlus",
                "weightMax",
                "tauPlus",
                "tauMinus",
                "isAdditive",
                "nestPostFirst",
                "nestExcludeZero",
                "nestCausalBoundary",
                "tickMs",
                ("axonalDelayTicks", "uint32_t"),
                ("postDelayTicks", "uint32_t"),
                ("maxSpikeTick", "uint32_t"),
            ],
            vars=[
                ("g", "scalar"),
                ("preTrace", "scalar"),
                ("postTrace", "scalar"),
                ("lastTraceTick", "uint32_t"),
                ("lastPostUpdateTick", "uint32_t"),
                ("lastPreUpdateTick", "uint32_t"),
            ],
            pre_neuron_var_refs=[("preSpikeTick", "uint32_t")],
            post_neuron_var_refs=[("postSpikeTick", "uint32_t")],
            pre_spike_syn_code="""
            assert((preSpikeTick > 0) && (preSpikeTick <= maxSpikeTick));
            assert(postSpikeTick <= maxSpikeTick);
            const uint32_t currentTick = preSpikeTick + axonalDelayTicks;
            const uint32_t postArrivalTick = postSpikeTick + postDelayTicks;
            assert(lastTraceTick <= currentTick);
            assert(lastPostUpdateTick <= currentTick);
            assert((postSpikeTick == 0) || (postArrivalTick <= currentTick));
            if (((nestPostFirst > 0.5) || (nestCausalBoundary > 0.5))
                && (postSpikeTick != 0) && (postArrivalTick == currentTick)
                && (lastPostUpdateTick != currentTick)) {
                const scalar postElapsed = (scalar)(currentTick - lastTraceTick) * tickMs;
                preTrace *= exp(-postElapsed / tauPlus);
                postTrace *= exp(-postElapsed / tauMinus);
                if (isAdditive > 0.5) {
                    g = fmax(0.0, fmin(weightMax,
                        g + (learningRate * weightMax * preTrace)));
                }
                else {
                    g += learningRate * pow(g, muPlus) * preTrace;
                }
                postTrace += 1.0;
                lastTraceTick = currentTick;
                lastPostUpdateTick = currentTick;
            }
            const scalar elapsed = (scalar)(currentTick - lastTraceTick) * tickMs;
            preTrace *= exp(-elapsed / tauPlus);
            postTrace *= exp(-elapsed / tauMinus);
            scalar effectivePostTrace = postTrace;
            if ((nestCausalBoundary > 0.5)
                && (lastPostUpdateTick == currentTick)) {
                effectivePostTrace -= 1.0;
            }
            if (isAdditive > 0.5) {
                g = fmax(0.0, fmin(weightMax,
                    g - (depressionRatio * learningRate * weightMax * effectivePostTrace)));
            }
            else {
                g = fmax(0.0,
                    g - (learningRate * depressionRatio * g * effectivePostTrace));
            }
            addToPost(epscInitial * deliveryScale * g);
            preTrace += 1.0;
            lastTraceTick = currentTick;
            lastPreUpdateTick = currentTick;
            """,
            post_spike_syn_code="""
            assert((postSpikeTick > 0) && (postSpikeTick <= maxSpikeTick));
            const uint32_t currentTick = postSpikeTick + postDelayTicks;
            assert(lastTraceTick <= currentTick);
            assert(lastPostUpdateTick <= currentTick);
            assert(lastPreUpdateTick <= currentTick);
            if (((nestPostFirst < 0.5) && (nestCausalBoundary < 0.5))
                || (lastPostUpdateTick != currentTick)) {
                const scalar elapsed = (scalar)(currentTick - lastTraceTick) * tickMs;
                preTrace *= exp(-elapsed / tauPlus);
                postTrace *= exp(-elapsed / tauMinus);
                scalar effectivePreTrace = preTrace;
                if ((nestExcludeZero > 0.5)
                    && (lastPreUpdateTick == currentTick)) {
                    effectivePreTrace -= 1.0;
                }
                if (isAdditive > 0.5) {
                    g = fmax(0.0, fmin(weightMax,
                        g + (learningRate * weightMax * effectivePreTrace)));
                }
                else {
                    g += learningRate * pow(g, muPlus) * effectivePreTrace;
                }
                postTrace += 1.0;
                lastTraceTick = currentTick;
                lastPostUpdateTick = currentTick;
            }
            """,
        )
        static_model = create_weight_update_model(
            "BrunelStaticPulse",
            params=["g", "epscInitial", "deliveryScale"],
            pre_spike_syn_code="addToPost(epscInitial * deliveryScale * g);",
        )
        from scipy.stats import binom

        no_autapse = api["create_sparse_connect_init_snippet"](
            "FixedIndegreeNoAutapse",
            params=[("num", "unsigned int")],
            col_build_code="""
            for (unsigned int c = num; c != 0; c--) {
                unsigned int idPre = gennrand() % (num_pre - 1);
                if (idPre >= id_post) {
                    idPre++;
                }
                addSynapse(idPre);
            }
            """,
            calc_max_row_len_func=lambda num_pre, num_post, pars: int(
                binom.ppf(
                    0.9999 ** (1.0 / num_pre),
                    int(pars["num"]) * num_post,
                    1.0 / (num_pre - 1),
                )
            ),
            calc_max_col_len_func=lambda unused_pre, unused_post, pars: int(pars["num"]),
        )
        scalar_dtype = np.float32 if precision == "float" else np.float64
        self.model = api["GeNNModel"](precision, f"brunel_{spec.rule.name}", backend=backend)
        self.model.dt = DT_MS
        self.model.seed = seed
        self.timing_enabled = timing_enabled
        self.model.timing_enabled = timing_enabled
        params = {
            "p11": p["p11"],
            "p21": p["p21"],
            "p22": p["p22"],
            "p31": p["p31"],
            "p32": p["p32"],
            "p33": p["p33"],
            "epscInitial": p["epsc_initial"],
            "externalMean": spec.external_rate_hz * DT_MS / 1000.0,
            "externalWeight": je_pa,
            "threshold": V_THRESHOLD_MV,
            "reset": V_RESET_MV,
            "refractorySteps": round(REFRACTORY_MS / DT_MS),
            "maxSpikeTick": self.max_spike_tick,
        }
        rng = np.random.default_rng(seed if state_seed is None else state_seed)
        self.exc = self.model.add_neuron_population(
            "Exc",
            spec.ne,
            neuron_model,
            params,
            {
                "V": rng.normal(VM_MEAN_MV, VM_STD_MV, spec.ne).astype(scalar_dtype),
                "Iex": 0.0,
                "dIex": 0.0,
                "Iin": 0.0,
                "dIin": 0.0,
                "refrac": 0,
                "spikeCount": 0,
                "tick": 0,
                "lastSpikeTick": 0,
            },
        )
        self.inh = self.model.add_neuron_population(
            "Inh",
            spec.ni,
            neuron_model,
            params,
            {
                "V": rng.normal(VM_MEAN_MV, VM_STD_MV, spec.ni).astype(scalar_dtype),
                "Iex": 0.0,
                "dIex": 0.0,
                "Iin": 0.0,
                "dIin": 0.0,
                "refrac": 0,
                "spikeCount": 0,
                "tick": 0,
                "lastSpikeTick": 0,
            },
        )
        if record_spikes:
            self.exc.spike_recording_enabled = True
            self.inh.spike_recording_enabled = True
        weight_max = spec.rule.weight_max_pa if spec.rule.weight_max_pa is not None else 1.0e30
        plastic_params = {
            "epscInitial": p["epsc_initial"],
            "deliveryScale": spec.recurrent_delivery_scale,
            "learningRate": spec.rule.learning_rate,
            "depressionRatio": spec.rule.depression_ratio,
            "muPlus": spec.rule.mu_plus,
            "weightMax": weight_max,
            "tauPlus": getattr(spec, "tau_plus_ms", TAU_PLUS_MS),
            "tauMinus": getattr(spec, "tau_minus_ms", TAU_MINUS_MS),
            "isAdditive": 1.0 if spec.rule.name == "additive" else 0.0,
            "nestPostFirst": 1.0 if stdp_tie_order == "nest_post_first" else 0.0,
            "nestExcludeZero": 1.0 if stdp_tie_order == "nest_exclude_zero" else 0.0,
            "nestCausalBoundary": 1.0
            if stdp_tie_order == "nest_causal_boundary"
            else 0.0,
            "tickMs": DT_MS,
            "axonalDelayTicks": delay_steps,
            "postDelayTicks": post_delay_steps,
            "maxSpikeTick": self.max_spike_tick,
        }
        trace_vars = {
            "g": je_pa,
            "preTrace": 0.0,
            "postTrace": 0.0,
            "lastTraceTick": 0,
            "lastPostUpdateTick": 0,
            "lastPreUpdateTick": 0,
        }
        self.ee = self.model.add_synapse_population(
            "EE",
            "SPARSE",
            self.exc,
            self.exc,
            init_weight_update(plastic_model, plastic_params, trace_vars,
                               pre_var_refs={"preSpikeTick": "lastSpikeTick"},
                               post_var_refs={"postSpikeTick": "lastSpikeTick"}),
            init_postsynaptic("DeltaCurr"),
            init_sparse_connectivity(no_autapse, {"num": spec.ce}) if connectivity is None else None,
        )
        self.ee.post_target_var = "exIn"
        self.ee.parallelism_hint = (
            api["ParallelismHint"].PRESYNAPTIC
            if ee_parallelism == "presynaptic"
            else api["ParallelismHint"].POSTSYNAPTIC
        )
        self.ee.num_threads_per_spike = ee_num_threads_per_spike
        self.ee.back_prop_delay_steps = post_delay_steps
        static_ex = {
            "g": je_pa,
            "epscInitial": p["epsc_initial"],
            "deliveryScale": spec.recurrent_delivery_scale,
        }
        static_in = {
            "g": -spec.rule.inhibitory_weight_ratio * je_pa,
            "epscInitial": p["epsc_initial"],
            "deliveryScale": spec.recurrent_delivery_scale,
        }
        self.ie = self.model.add_synapse_population(
            "IE",
            "SPARSE",
            self.inh,
            self.exc,
            init_weight_update(static_model, static_in),
            init_postsynaptic("DeltaCurr"),
            init_sparse_connectivity("FixedNumberPreWithReplacement", {"num": spec.ci}) if connectivity is None else None,
        )
        self.ie.post_target_var = "inIn"
        self.ei = self.model.add_synapse_population(
            "EI",
            "SPARSE",
            self.exc,
            self.inh,
            init_weight_update(static_model, static_ex),
            init_postsynaptic("DeltaCurr"),
            init_sparse_connectivity("FixedNumberPreWithReplacement", {"num": spec.ce}) if connectivity is None else None,
        )
        self.ei.post_target_var = "exIn"
        self.ii = self.model.add_synapse_population(
            "II",
            "SPARSE",
            self.inh,
            self.inh,
            init_weight_update(static_model, static_in),
            init_postsynaptic("DeltaCurr"),
            init_sparse_connectivity(no_autapse, {"num": spec.ci}) if connectivity is None else None,
        )
        self.ii.post_target_var = "inIn"
        for synapses in (self.ee, self.ie, self.ei, self.ii):
            synapses.axonal_delay_steps = delay_steps
        if connectivity is not None:
            if set(connectivity) != {"ee", "ei", "ie", "ii"}:
                raise ValueError("checkpoint connectivity must contain EE, EI, IE and II")
            for name, (pre, post) in connectivity.items():
                getattr(self, name).set_sparse_connections(pre, post)
        model_code = f"brunel_{spec.rule.name}_CODE"
        if reuse_build is not None:
            build_path = reuse_build.resolve()
            if not (build_path / model_code / "librunner.so").is_file():
                raise ValueError(f"invalid reusable GeNN build: {build_path}")
        else:
            build_path.mkdir(parents=True, exist_ok=False)
        build_started = time.perf_counter()
        self.model.build(
            str(build_path.resolve()), never_rebuild=reuse_build is not None
        )
        self.build_wall_seconds = time.perf_counter() - build_started
        if record_spikes:
            self.model.load(num_recording_timesteps=recording_steps)
        else:
            self.model.load()
        if collect_connectivity_stats:
            self.ee.pull_connectivity_from_device()
            for synapses in (self.ee, self.ie, self.ei, self.ii):
                synapses._row_lengths.pull_from_device()
            self._outdegrees = {
                name: np.asarray(synapses._row_lengths.view, dtype=np.uint64).copy()
                for name, synapses in (
                    ("ee", self.ee),
                    ("ie", self.ie),
                    ("ei", self.ei),
                    ("ii", self.ii),
                )
            }
        else:
            self._outdegrees = None
        self._record_spikes = record_spikes

    def close(self) -> None:
        self.model.unload()

    def kernel_timing(self) -> dict[str, float] | None:
        if not self.timing_enabled:
            return None
        return {
            "neuron_update_seconds": self.model.neuron_update_time,
            "presynaptic_update_seconds": self.model.presynaptic_update_time,
            "postsynaptic_update_seconds": self.model.postsynaptic_update_time,
        }

    def step(self, steps: int, *, synchronize: bool = True) -> float:
        started = time.perf_counter()
        for _ in range(steps):
            self.model.step_time()
        if synchronize and not self.timing_enabled:
            self.exc.vars["spikeCount"].pull_from_device()
        return time.perf_counter() - started

    def spike_counts(self, n_record: int) -> np.ndarray:
        variable = self.exc.vars["spikeCount"]
        if self.timing_enabled:
            variable.pull_from_device()
        return np.asarray(variable.view[:n_record], dtype=np.uint64).copy()

    def population_spike_counts(self) -> tuple[np.ndarray, np.ndarray]:
        self.exc.vars["spikeCount"].pull_from_device()
        self.inh.vars["spikeCount"].pull_from_device()
        return (
            np.asarray(self.exc.vars["spikeCount"].view, dtype=np.uint64).copy(),
            np.asarray(self.inh.vars["spikeCount"].view, dtype=np.uint64).copy(),
        )

    def event_counters(
        self,
        baseline: tuple[np.ndarray, np.ndarray],
        final: tuple[np.ndarray, np.ndarray],
    ) -> dict[str, Any]:
        if self._outdegrees is None:
            raise RuntimeError("connectivity statistics were disabled for this model")
        exc = final[0] - baseline[0]
        inh = final[1] - baseline[1]
        pre = {
            "ee": int(np.dot(exc, self._outdegrees["ee"])),
            "ie": int(np.dot(inh, self._outdegrees["ie"])),
            "ei": int(np.dot(exc, self._outdegrees["ei"])),
            "ii": int(np.dot(inh, self._outdegrees["ii"])),
        }
        return {
            "excitatory_spikes": int(exc.sum()),
            "inhibitory_spikes": int(inh.sum()),
            "total_spikes": int(exc.sum() + inh.sum()),
            "structural_synapses": {
                name: int(outdegree.sum())
                for name, outdegree in self._outdegrees.items()
            },
            "presynaptic_traversals_enqueued": pre,
            "ee_postsynaptic_plastic_traversals": int(exc.sum())
            * int(self._outdegrees["ee"].sum() // exc.size),
        }

    def sample_weights(self, requested: int) -> np.ndarray:
        variable = self.ee.vars["g"]
        variable.pull_from_device()
        values = np.asarray(variable.values, dtype=np.float64)
        count = min(requested, values.size)
        indices = np.linspace(0, values.size - 1, count, dtype=np.int64)
        return values[indices].copy()

    def recorded_spikes(self, n_record: int) -> tuple[np.ndarray, np.ndarray]:
        times, ids = self.recorded_population_spikes()["excitatory"]
        keep = ids < n_record
        return times[keep], ids[keep]

    def recorded_population_spikes(
        self,
    ) -> dict[str, tuple[np.ndarray, np.ndarray]]:
        if not self._record_spikes:
            raise RuntimeError("spike recording was disabled for this model")
        self.model.pull_recording_buffers_from_device()
        result = {}
        for name, population in (("excitatory", self.exc), ("inhibitory", self.inh)):
            times, ids = population.spike_recording_data[0]
            result[name] = (
                np.asarray(times, dtype=np.float64).copy(),
                np.asarray(ids, dtype=np.int64).copy(),
            )
        return result


def run(args: argparse.Namespace) -> int:
    api = _import_genn()
    spec = make_model(
        args.rule,
        args.network_scale,
        args.indegree_scale,
        delay_ms=args.delay_ms,
        recurrent_delivery_scale=args.recurrent_delivery_scale,
        external_rate_scale=args.external_rate_scale,
    )
    backend_name = f"genn-{args.backend}"
    manifest = base_manifest(backend_name, spec)
    manifest.update(
        {
            "genn_version": api["module"].__version__,
            "precision": args.precision,
            "ee_parallelism": args.ee_parallelism,
            "ee_num_threads_per_spike": args.ee_num_threads_per_spike,
            "seed": args.seed,
            "state_seed": args.state_seed if args.state_seed is not None else args.seed,
            "presim_ms": args.presim_ms,
            "sim_ms": args.sim_ms,
            "chunk_ms": args.chunk_ms,
            "abort_rate_hz": args.abort_rate_hz,
            "connectivity": "procedural fixed indegree with replacement; recurrent autapses excluded",
            "external_input": "independent Poisson multiplicity sampled in each neuron kernel",
            "stdp_timing": args.stdp_timing,
            "stdp_tie_order": args.stdp_tie_order,
            **stdp_timestamp_metadata(),
            "stdp_post_path_delay_ms": stdp_post_path_delay_ms(
                args.stdp_timing, spec.delay_ms
            ),
            "genn_timing_enabled": args.genn_timing,
            "profile_accounting_enabled": args.profile_accounting,
            "reuse_build": str(args.reuse_build.resolve()) if args.reuse_build else None,
        }
    )
    create_output(args.output, manifest)
    program_started = time.perf_counter()
    n_record = min(args.record_neurons, spec.ne)
    total_steps = round((args.presim_ms + args.sim_ms) / DT_MS)
    construction_started = time.perf_counter()
    network = GeNNBrunel(
        spec=spec,
        seed=args.seed,
        state_seed=args.state_seed,
        backend=args.backend,
        build_path=args.output / "build" if args.reuse_build is None else args.reuse_build,
        recording_steps=total_steps,
        precision=args.precision,
        ee_parallelism=args.ee_parallelism,
        ee_num_threads_per_spike=args.ee_num_threads_per_spike,
        stdp_timing=args.stdp_timing,
        stdp_tie_order=args.stdp_tie_order,
        timing_enabled=args.genn_timing,
        reuse_build=args.reuse_build,
    )
    construction_wall = time.perf_counter() - construction_started
    initial_weights = network.sample_weights(args.weight_sample_size)
    initial_stats = weight_stats(initial_weights, spec.rule)
    print(
        f"CONFIG backend={backend_name} rule={args.rule} dt_ms={DT_MS} "
        f"delay_ms={spec.delay_ms} recurrent_delivery_scale={spec.recurrent_delivery_scale} "
        f"external_rate_scale={spec.external_rate_scale} "
        f"ne={spec.ne} ni={spec.ni} ce={spec.ce} ci={spec.ci} precision={args.precision} "
        f"ee_parallelism={args.ee_parallelism} "
        f"ee_threads_per_spike={args.ee_num_threads_per_spike} "
        f"recurrent_synapses={spec.recurrent_synapses} tau_plus_ms={TAU_PLUS_MS} "
        f"tau_minus_ms={TAU_MINUS_MS}",
        flush=True,
    )
    presim_steps = round(args.presim_ms / DT_MS)
    presim_wall = network.step(presim_steps)
    presim_kernel_timing = network.kernel_timing()
    population_baseline = network.population_spike_counts()
    baseline_counts = network.spike_counts(n_record)
    previous_counts = baseline_counts.copy()
    elapsed_ms = 0.0
    simulation_wall = 0.0
    periodic = []
    termination_reason = "requested_duration_completed"
    diagnostics_wall = 0.0
    try:
        while elapsed_ms < args.sim_ms - 1e-12:
            duration_ms = min(args.chunk_ms, args.sim_ms - elapsed_ms)
            steps = round(duration_ms / DT_MS)
            call_wall = network.step(steps)
            simulation_wall += call_wall
            elapsed_ms += duration_ms
            counts = network.spike_counts(n_record)
            chunk_events = int(np.sum(counts - previous_counts))
            cumulative_events = int(np.sum(counts - baseline_counts))
            previous_counts = counts
            diagnostics_started = time.perf_counter()
            sampled = network.sample_weights(args.weight_sample_size)
            stats = weight_stats(sampled, spec.rule)
            if args.profile_accounting:
                diagnostics_wall += time.perf_counter() - diagnostics_started
            row = {
                "elapsed_ms": elapsed_ms,
                "chunk_ms": duration_ms,
                "chunk_wall_seconds": call_wall,
                "chunk_seconds_per_step": call_wall / steps,
                "chunk_rate_hz": chunk_events / (n_record * duration_ms) * 1000.0,
                "cumulative_rate_hz": cumulative_events / (n_record * elapsed_ms) * 1000.0,
                "weight_mean": stats["mean"],
                "weight_std": stats["std"],
                "weight_boundary_fraction": stats["boundary_fraction"],
            }
            periodic.append(row)
            print("PERIODIC " + json.dumps(row, sort_keys=True), flush=True)
            if args.abort_rate_hz is not None and row["chunk_rate_hz"] >= args.abort_rate_hz:
                termination_reason = (
                    f"chunk_rate_hz={row['chunk_rate_hz']} reached abort_rate_hz={args.abort_rate_hz}"
                )
                print("EARLY_STOP " + termination_reason, flush=True)
                break
        final_weights = network.sample_weights(args.weight_sample_size)
        population_final = network.population_spike_counts()
        counters = network.event_counters(population_baseline, population_final)
        total_kernel_timing = network.kernel_timing()
        final_stats = weight_stats(final_weights, spec.rule)
        if termination_reason == "requested_duration_completed":
            times, ids = network.recorded_spikes(n_record)
            times -= args.presim_ms
            keep = (times >= 0.0) & (times <= elapsed_ms + DT_MS * 0.5)
            final_spikes = spike_stats(times[keep], ids[keep], elapsed_ms, n_record)
        else:
            final_counts = network.spike_counts(n_record) - baseline_counts
            final_spikes = {
                "rate_hz": float(np.sum(final_counts) / (n_record * elapsed_ms) * 1000.0),
                "population_fano_3ms": None,
                "mean_cv_isi": None,
                "incomplete_recording_reason": (
                    "GeNN recording buffers are only readable after their configured duration"
                ),
            }
    finally:
        network.close()
    simulation_steps = round(elapsed_ms / DT_MS)
    if total_kernel_timing is None:
        simulation_kernel_timing = None
    else:
        simulation_kernel_timing = {
            key: value - presim_kernel_timing[key]
            for key, value in total_kernel_timing.items()
        }
    timing = {
        "construction_wall_seconds": construction_wall,
        "genn_build_wall_seconds": network.build_wall_seconds,
        "presimulation_wall_seconds": presim_wall,
        "simulation_wall_seconds": simulation_wall,
        "whole_program_wall_seconds": time.perf_counter() - program_started,
        "presimulation_steps": presim_steps,
        "simulation_steps": simulation_steps,
        "requested_simulation_steps": round(args.sim_ms / DT_MS),
        "actual_simulation_ms": elapsed_ms,
        "requested_simulation_ms": args.sim_ms,
        "simulation_seconds_per_step": simulation_wall / simulation_steps,
        "combined_simulate_call_seconds_per_step": (presim_wall + simulation_wall)
        / (presim_steps + simulation_steps),
        "genn_kernel_seconds": simulation_kernel_timing,
        "periodic_diagnostics_wall_seconds": diagnostics_wall
        if args.profile_accounting
        else None,
    }
    results = {
        "manifest": manifest,
        "timing": timing,
        "initial_weight_stats": initial_stats,
        "final_weight_stats": final_stats,
        "spike_stats": final_spikes,
        "periodic": periodic,
        "event_counters": counters,
        "termination": {
            "completed_requested_duration": termination_reason
            == "requested_duration_completed",
            "reason": termination_reason,
        },
    }
    with (args.output / "results.json").open("x", encoding="ascii") as stream:
        json.dump(results, stream, indent=2, sort_keys=True)
        stream.write("\n")
    np.savez_compressed(args.output / "weight_samples.npz", initial=initial_weights, final=final_weights)
    print(
        "RESULT "
        + json.dumps(
            {"timing": timing, "spike_stats": final_spikes, "final_weight_stats": final_stats},
            sort_keys=True,
        ),
        flush=True,
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="GeNN port of the measured Brunel STDP models")
    parser.add_argument("--rule", choices=("additive", "morrison"), required=True)
    parser.add_argument("--backend", default="single_threaded_cpu")
    parser.add_argument("--precision", choices=("double", "float"), default="float")
    parser.add_argument(
        "--ee-parallelism",
        choices=("postsynaptic", "presynaptic"),
        default="postsynaptic",
    )
    parser.add_argument("--ee-num-threads-per-spike", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260724)
    parser.add_argument("--state-seed", type=int)
    parser.add_argument("--network-scale", type=float, default=1.0)
    parser.add_argument(
        "--indegree-scale", type=float, default=GENN_DEFAULT_INDEGREE_SCALE
    )
    parser.add_argument("--delay-ms", type=float, default=GENN_DEFAULT_DELAY_MS)
    parser.add_argument(
        "--recurrent-delivery-scale",
        type=float,
        default=GENN_DEFAULT_RECURRENT_DELIVERY_SCALE,
    )
    parser.add_argument(
        "--external-rate-scale",
        type=float,
        help="scale external drive (default: 0.47 additive, 0.32 Morrison)",
    )
    parser.add_argument("--presim-ms", type=float, default=100.0)
    parser.add_argument("--sim-ms", type=float, default=1000.0)
    parser.add_argument("--chunk-ms", type=float, default=100.0)
    parser.add_argument("--abort-rate-hz", type=float, default=100.0)
    parser.add_argument("--record-neurons", type=int, default=1000)
    parser.add_argument("--weight-sample-size", type=int, default=100000)
    parser.add_argument("--stdp-timing", choices=STDP_TIMING_MODES, default="arrival")
    parser.add_argument(
        "--stdp-tie-order", choices=STDP_TIE_MODES, default="nest_causal_boundary"
    )
    parser.add_argument(
        "--genn-timing",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable GeNN's per-stage CUDA event timers",
    )
    parser.add_argument(
        "--profile-accounting",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="time host-side periodic diagnostic work",
    )
    parser.add_argument(
        "--reuse-build",
        type=Path,
        help="load an existing immutable GeNN build directory without compiling",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.external_rate_scale is None:
        args.external_rate_scale = GENN_DEFAULT_EXTERNAL_RATE_SCALES[args.rule]
    for name in (
        "network_scale",
        "indegree_scale",
        "recurrent_delivery_scale",
        "external_rate_scale",
        "sim_ms",
        "chunk_ms",
    ):
        if getattr(args, name) <= 0.0:
            raise SystemExit(f"--{name.replace('_', '-')} must be positive")
    if args.presim_ms < 0.0:
        raise SystemExit("--presim-ms must be non-negative")
    if args.delay_ms < 0.0:
        raise SystemExit("--delay-ms must be non-negative")
    if args.ee_num_threads_per_spike <= 0:
        raise SystemExit("--ee-num-threads-per-spike must be positive")
    if args.ee_parallelism == "postsynaptic" and args.ee_num_threads_per_spike != 1:
        raise SystemExit(
            "--ee-num-threads-per-spike only applies to presynaptic EE parallelism"
        )
    for name in ("delay_ms", "presim_ms", "sim_ms", "chunk_ms"):
        steps = getattr(args, name) / DT_MS
        if not math.isclose(steps, round(steps), abs_tol=1e-9):
            raise SystemExit(f"--{name.replace('_', '-')} must be an integer number of timesteps")
    return run(args)
