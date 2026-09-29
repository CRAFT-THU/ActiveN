"""Import an after-synapses/before-neurons GeNN checkpoint without presimulation."""
import hashlib
import json
from pathlib import Path

import numpy as np

from brunel import NE, NI, F, constants


def convert(data, rule, seed):
    meta = json.loads(str(data["metadata"]))
    if (meta.get("format") != "brunel-genn-phase-checkpoint-v1"
            or meta.get("boundary") != "after-synapses-before-neurons"
            or meta.get("rule") != rule or meta.get("dt_ms") != .1
            or meta.get("delay_steps") != 0):
        raise ValueError("incompatible GeNN checkpoint contract")
    n = np.zeros((NE+NI,20),np.uint32)
    f = n.view(F)
    for name,start,count in (("exc",0,NE),("inh",NE,NI)):
        for col,key in enumerate(("V","Iex","dIex","Iin","dIin")):
            values = np.asarray(data[f"{name}_{key}"],F).reshape(-1)
            if values.shape != (count,) or not np.all(np.isfinite(values)):
                raise ValueError(f"invalid {name} {key}")
            f[start:start+count,col] = values
        n[start:start+count,5] = data[f"{name}_refrac"]
        if np.any(n[start:start+count,5] > 5):
            raise ValueError("invalid refractory state")
        if np.any(data[f"{name}_tick"] != meta["timestep"]):
            raise ValueError("neuron clock does not match checkpoint boundary")
    n[:,11]=np.arange(NE+NI)
    # ActiveN owns a different RNG. Preserve its established per-SMT algorithm,
    # explicitly seeding a branched future rather than claiming exact replay.
    n[:,8]=np.random.default_rng(seed).integers(1,2**32,size=NE+NI,dtype=np.uint32)
    for col,key in ((6,"canonical_pre_trace"),(7,"canonical_post_trace")):
        values=np.asarray(data[key],F)
        if values.shape!=(NE,) or not np.all(np.isfinite(values)) or np.any(values<0):
            raise ValueError("invalid canonical trace")
        f[:NE,col]=values
    f[:NE,17]=np.asarray(data["ee_input"]).reshape(-1)
    f[:NE,18]=np.asarray(data["ie_input"]).reshape(-1)
    f[NE:,17]=np.asarray(data["ei_input"]).reshape(-1)
    f[NE:,18]=np.asarray(data["ii_input"]).reshape(-1)
    edges={}
    for name,ns,nt,degree,src,dst in (("ee",NE,NE,450,0,0),("ei",NE,NI,450,0,NE),
                                      ("ie",NI,NE,112,NE,0),("ii",NI,NI,112,NE,NE)):
        pre=np.asarray(data[f"{name}_pre"],np.int32)
        post=np.asarray(data[f"{name}_post"],np.int32)
        if (pre.shape!=post.shape or len(pre)!=nt*degree or np.any(pre<0) or np.any(pre>=ns)
                or np.any(post<0) or np.any(post>=nt)
                or not np.all(np.bincount(post,minlength=nt)==degree)
                or (src==dst and np.any(pre==post))):
            raise ValueError(f"invalid {name} multigraph")
        edges[name]=(pre+src,post+dst)
    pre,post=edges["ee"]
    order=np.argsort(pre,kind="stable")
    c=constants(rule)
    # One rounding after folding the delivery scale into the weight matrix.
    scale=float(c["epsc"])*float(c["delivery"])
    w=F(np.asarray(data["ee_g"],np.float64)[order]*scale)
    if not np.all(np.isfinite(w)) or np.any(w<0):
        raise ValueError("invalid plastic weights")
    if rule=="additive" and np.any(w>np.nextafter(c["wmax"],F(np.inf))):
        raise ValueError("additive weight exceeds bound")
    sp=np.concatenate([edges[g][0] for g in ("ei","ie","ii")])
    st=np.concatenate([edges[g][1] for g in ("ei","ie","ii")])
    so=np.argsort(sp,kind="stable")
    return (pre[order],post[order],sp[so],st[so]),(n,w),meta


def load_checkpoint(path,rule,seed):
    path=Path(path)
    with np.load(path,allow_pickle=False) as file:
        result=convert(file,rule,seed)
    edges,state,meta=result
    provenance=dict(meta,checkpoint_path=str(path.resolve()),
                    checkpoint_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    future_rng="ActiveN per-SMT xorshift32; independent of GeNN XORWOW")
    return edges,state,provenance
