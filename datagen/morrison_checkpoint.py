"""Read-only native checkpoint transport; identical local two-pass conversion."""
from pathlib import Path
import base64, contextlib, hashlib, json, os, shlex, shutil, struct, subprocess, sys, time
from dataclasses import asdict
import numpy as np
DATAGEN=Path(__file__).resolve().parent
R=DATAGEN.parent
if 'morrison_native' not in sys.modules: sys.path.insert(0,str(DATAGEN))
from morrison_native import manifest, arrays, real_slots
from morrison_graph import sha256, checkpoint_clock, canonical_traces, MAX_ROW_EDGES
from morrison_model import Spec, GROUPS, hot_state, constants
MAGIC=b'MOR750K1';END=0xffffffff

def save(path,value): Path(path).write_text(json.dumps(value,indent=2)+'\n')
def b64(b): return base64.b64encode(b).decode('ascii')
def un64(s): return base64.b64decode(s,validate=True)

def shapes(spec):
    g={}
    for name,(npre,_,_,_,_) in spec.projections().items():
        g[name+'_rowLength']=('<u4',(npre,));g[name+'_ind']=('<u4',(None,))
    g.update(ee_colLength=('<u4',(spec.ne,)),ee_remap=('<u4',(spec.ne*spec.ce,)))
    return g

def geometry(path,spec):
    meta=manifest(path/'manifest.json','graph'); data=arrays(path,meta,shapes(spec));strides={}
    for name,(npre,npost,degree,_,_) in spec.projections().items():
        rows,ind=data[name+'_rowLength'],data[name+'_ind'];stride,rem=divmod(len(ind),npre)
        if rem or not 0<stride<=MAX_ROW_EDGES or np.any(rows>stride) or int(rows.sum(dtype=np.uint64))!=npost*degree:
            raise ValueError('invalid graph geometry '+name)
        strides[name]=stride
    if np.any(data['ee_colLength']!=spec.ce): raise ValueError('invalid column degrees')
    return meta,data,strides

def contract(meta,spec):
    c=meta.get('compatibility',{})
    if (meta.get('boundary')!='after-synapses-before-neurons' or meta.get('dt_ms')!=.1
        or c.get('scalar_dtype')!='<f4' or c.get('dt_ms')!=.1
        or c.get('axonal_delay_steps')!=dict.fromkeys(GROUPS,0) or c.get('post_delay_steps')!=0
        or c.get('stdp_timing')!='arrival' or c.get('stdp_tie_order')!='nest_causal_boundary'):
        raise ValueError('native boundary/semantics mismatch')
    for key,val in spec.model().items():
        if meta.get('model',{}).get(key)!=val: raise ValueError('model mismatch '+key)

def exporter(state_path,graph_path,expected,spec,pass_id):
    # The entire execution on CRAFT-A100 is read-only. No caches or files created.
    state_path=Path(state_path);graph_path=Path(graph_path)
    if pass_id not in (1,2): raise ValueError('pass')
    sm=state_path/'manifest.json';gm=graph_path/'manifest.json'
    meta=manifest(sm,'state');contract(meta,spec)
    if sha256(sm)!=expected or sha256(gm)!=meta['graph_manifest_sha256'] or (state_path/meta['graph']).resolve()!=graph_path.resolve():
        raise ValueError('checkpoint/graph manifest binding')
    paths=[sm,gm]+list(state_path.glob('*.npy'))+list(graph_path.glob('*.npy'))
    stat=lambda p:(p.stat().st_size,p.stat().st_mtime_ns,p.stat().st_ctime_ns)
    before={str(p):stat(p) for p in paths}
    with contextlib.redirect_stdout(sys.stderr):
        gmeta,g,strides=geometry(graph_path,spec)
        want={}
        for pop,n in (('exc',spec.ne),('inh',spec.ni)):
            for field in ('V','Iex','Iin','dIex','dIin'):want[pop+'_'+field]=('<f4',(n,))
            for field in ('refrac','tick','lastSpikeTick','spikeCount'):want[pop+'_'+field]=('<u4',(n,))
            want[pop+'_rng']=('<u4',(n,6))
        for field in ('g','preTrace','postTrace','lastTraceTick','lastPreUpdateTick','lastPostUpdateTick'):
            want['ee_'+field]=('<u4' if field.startswith('last') else '<f4',g['ee_ind'].shape)
        for name,(_,npost,_,_,_) in spec.projections().items():want[name+'_outPost']=('<f4',(1,npost))
        a=arrays(state_path,meta,want);tick=checkpoint_clock(meta,a,spec)
    small={key:dict(dtype=v.dtype.str,shape=list(v.shape),bytes=b64(v.tobytes()),raw_sha256=hashlib.sha256(v).hexdigest())
           for key,v in a.items() if key.endswith('_outPost') or key.startswith(('exc_','inh_'))}
    h=hashlib.sha256();byte_count=0
    out=sys.stdout.buffer
    def write(b):
        nonlocal byte_count
        out.write(b);h.update(b);byte_count+=len(b)
    header=dict(format='morrison-raw-real-edges-v1',pass_id=pass_id,spec=asdict(spec),state_manifest=b64(sm.read_bytes()),graph_manifest=b64(gm.read_bytes()),tick=tick,strides=strides,small=small,numpy=np.__version__,python=sys.version)
    blob=json.dumps(header,separators=(',',':')).encode();write(MAGIC+struct.pack('<I',len(blob)));write(blob)
    count=0;nb=0
    names=(['ee_g'] if pass_id==1 else [])+['ee_preTrace','ee_postTrace','ee_lastTraceTick']
    for seq,(_,slots) in enumerate(real_slots(g['ee_rowLength'],strides['ee'])):
        words=np.empty((len(slots),len(names)),dtype='<u4')
        for col,name in enumerate(names): words[:,col]=a[name][slots].view('<u4')
        write(struct.pack('<II',seq,len(slots)));write(words.tobytes());count+=len(slots);nb+=1
        if seq%100==0:print(f'pass={pass_id} block={seq} real_edges={count}',file=sys.stderr,flush=True)
    if count!=spec.ne*spec.ce:raise ValueError('incomplete edges')
    if any(stat(Path(p))!=v for p,v in before.items()) or sha256(sm)!=expected or sha256(gm)!=meta['graph_manifest_sha256']:
        raise ValueError('source changed during export')
    trailer=dict(real_edges=count,blocks=nb,stream_bytes=byte_count,stream_sha256=h.hexdigest(),state_sha256=expected,graph_sha256=sha256(gm),source_stats_unchanged=True)
    blob=json.dumps(trailer).encode();out.write(struct.pack('<II',END,len(blob)));out.write(blob);out.flush()
    print(json.dumps(trailer),file=sys.stderr,flush=True)

def bundle(path):
    parts=['import types, sys\n']
    for name in ('morrison_model','morrison_graph','morrison_native'):
        src=(DATAGEN/f'{name}.py').read_text()
        parts.append(f"m=types.ModuleType({name!r});m.__file__={str(DATAGEN/f'{name}.py')!r};sys.modules[{name!r}]=m;exec(compile({src!r},m.__file__,'exec'),m.__dict__)\n")
    parts.append(Path(__file__).read_text());Path(path).write_text(''.join(parts))

class Reader:
    def __init__(self,stream):self.stream=stream;self.h=hashlib.sha256();self.count=0
    def get(self,n,hashed=True):
        parts=[];left=n
        while left:
            b=self.stream.read(left)
            if not b:raise EOFError(f'stream truncated: {left} bytes absent')
            parts.append(b);left-=len(b)
        value=b''.join(parts)
        if hashed:self.h.update(value);self.count+=n
        return value

def reduce_checkpoint(state_path,graph_path,out,spec,expected,host=None,remote_graph=None):
    out=Path(out);out.mkdir(exist_ok=False);graph_path=Path(graph_path)
    rec=dict(start=time.time(),state_path=str(state_path),graph_path=str(graph_path),remote_graph=remote_graph,host=host,spec=asdict(spec),expected=expected,passes=[],complete=False,local_numpy=np.__version__)
    save(out/'import.json',rec)
    bundle_path=out/'exporter-bundle.py';bundle(bundle_path);rec['bundle_sha256']=sha256(bundle_path)
    def guard():
        if shutil.disk_usage(out).free<(2<<30):raise RuntimeError('import disk headroom <2GiB')
    try:
        gmeta,g,strides=geometry(graph_path,spec)
        # Reconstruct the original Graph hash without writing canonical index arrays.
        gh=hashlib.sha256()
        for name,(npre,npost,degree,srcbase,dstbase) in spec.projections().items():
            counts=np.zeros(npost,np.int64)
            for pre,slots in real_slots(g[name+'_rowLength'],strides[name]):
                post=g[name+'_ind'][slots]
                if np.any(post>=npost) or (srcbase==dstbase and np.any(pre==post)):raise ValueError('invalid IDs/autapse')
                counts+=np.bincount(post,minlength=npost);gh.update(pre.tobytes())
            if np.any(counts!=degree):raise ValueError('fixed indegree mismatch')
            for _,slots in real_slots(g[name+'_rowLength'],strides[name]):gh.update(g[name+'_ind'][slots].tobytes())
        rec['canonical_graph_sha256']=gh.hexdigest();rec['graph_manifest_sha256']=sha256(graph_path/'manifest.json')
        guard();weights=np.lib.format.open_memmap(out/'ee_g.partial.npy',mode='w+',dtype='<f4',shape=(spec.ne*spec.ce,))
        tick=None;native_small=None;pass_id=0
        def blocks():
            nonlocal pass_id,tick,native_small
            pass_id+=1;pid=pass_id
            args=['--export',str(state_path),str(remote_graph or graph_path),expected,json.dumps(asdict(spec),separators=(',',':')),str(pid)]
            if host:
                argv=['ssh','-T','-o','BatchMode=yes','-o','ConnectTimeout=10','-o','StrictHostKeyChecking=yes','-o','UpdateHostKeys=no',host,
                      'OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 python3 -B -u - '+shlex.join(args)]
                stdin=bundle_path.open('rb')
            else:argv=[sys.executable,'-B','-u',str(bundle_path),*args];stdin=subprocess.DEVNULL
            pr=dict(argv=argv,cwd=str(R),start=time.time(),environment_overrides=dict(OMP_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1'),stderr=str(out/f'pass{pid}.stderr.log'))
            rec['passes'].append(pr);save(out/'import.json',rec)
            env=dict(os.environ,OMP_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1')
            with (out/f'pass{pid}.stderr.log').open('wb') as err:
                child=subprocess.Popen(argv,cwd=R,env=env,stdin=stdin,stdout=subprocess.PIPE,stderr=err)
                if host:stdin.close()
                rd=Reader(child.stdout)
                try:
                    magic,n=struct.unpack('<8sI',rd.get(12));assert magic==MAGIC and n<32<<20
                    hdr=json.loads(rd.get(n));save(out/f'pass{pid}.header.json',hdr)
                    smb=un64(hdr['state_manifest']);gmb=un64(hdr['graph_manifest']);meta=json.loads(smb)
                    assert hashlib.sha256(smb).hexdigest()==expected and gmb==(graph_path/'manifest.json').read_bytes()
                    assert meta['graph_manifest_sha256']==rec['graph_manifest_sha256'] and hdr['spec']==asdict(spec) and hdr['pass_id']==pid and hdr['strides']==strides
                    contract(meta,spec)
                    small={}
                    for key,v in hdr['small'].items():
                        raw=un64(v['bytes']);assert hashlib.sha256(raw).hexdigest()==v['raw_sha256']
                        small[key]=np.frombuffer(raw,dtype=v['dtype']).reshape(v['shape'])
                    observed_tick=checkpoint_clock(meta,small,spec)
                    assert hdr['tick']==observed_tick
                    if pid==1:
                        tick=observed_tick;native_small=small
                        (out/'state-manifest.json').write_bytes(smb);(out/'graph-manifest.json').write_bytes(gmb)
                        sd=out/'native-small';sd.mkdir()
                        for key,v in small.items():np.save(sd/f'{key}.npy',v)
                    else:
                        assert observed_tick==tick and smb==(out/'state-manifest.json').read_bytes()
                        assert small.keys()==native_small.keys()
                        for key,v in small.items():assert np.array_equal(v.view('<u4'),native_small[key].view('<u4'))
                    cursor=0;nb=0
                    for seq,(pre,slots) in enumerate(real_slots(g['ee_rowLength'],strides['ee'])):
                        a,n=struct.unpack('<II',rd.get(8));assert a==seq and n==len(slots)
                        words=np.frombuffer(rd.get(n*(16 if pid==1 else 12)),dtype='<u4').reshape(n,-1)
                        if pid==1:
                            raw=words[:,0].view('<f4')
                            if np.any(~np.isfinite(raw)) or np.any(raw<0) or np.any(~np.isfinite((raw.astype(np.float64)*float(constants()['epsc'])).astype(np.float32))):raise ValueError('invalid real weight')
                            weights[cursor:cursor+n]=raw
                        i=1 if pid==1 else 0
                        # canonical_traces asks for tick before first iteration. Set via manifest below.
                        yield pre,g['ee_ind'][slots],words[:,i+2],words[:,i].view('<f4'),words[:,i+1].view('<f4')
                        cursor+=n;nb+=1
                        if seq%100==0:guard();print(f'import pass={pid} edges={cursor}',flush=True)
                    a,n=struct.unpack('<II',rd.get(8,False));assert a==END and n<1<<20
                    trailer=json.loads(rd.get(n,False));assert not child.stdout.read(1)
                    assert trailer['real_edges']==cursor==spec.ne*spec.ce and trailer['blocks']==nb
                    assert trailer['stream_bytes']==rd.count and trailer['stream_sha256']==rd.h.hexdigest()
                    assert trailer['state_sha256']==expected and trailer['graph_sha256']==rec['graph_manifest_sha256'] and trailer['source_stats_unchanged']
                    pr.update(trailer=trailer,received_sha256=rd.h.hexdigest(),received_bytes=rd.count,exit_code=child.wait(),end=time.time())
                    assert pr['exit_code']==0
                finally:
                    child.stdout.close()
                    if child.poll() is None:child.terminate();child.wait()
                    pr.setdefault('exit_code',child.returncode);pr.setdefault('end',time.time());save(out/'import.json',rec)
        # First next() opens/validates the header and exact integer checkpoint clock.
        first=blocks();first_block=next(first)
        from itertools import chain
        calls=0
        def trace_blocks():
            nonlocal calls
            calls+=1
            return chain((first_block,),first) if calls==1 else blocks()
        traces,errors=canonical_traces(spec,tick,trace_blocks)
        assert pass_id==2
        for name in GROUPS:native_small[name+'_input']=native_small[name+'_outPost']
        neurons=hot_state(native_small,spec,traces);np.save(out/'neurons.npy',neurons)
        for name,t in traces.items():np.save(out/f'{name}-trace.npy',t)
        weights.flush();del weights;os.replace(out/'ee_g.partial.npy',out/'ee_g.npy')
        rec.update(tick=tick,canonical_trace_error=errors,weights_sha256=sha256(out/'ee_g.npy'),neurons_sha256=sha256(out/'neurons.npy'),real_edges=spec.ne*spec.ce,complete=True,end=time.time(),future_rng='ActiveN per-SMT xorshift32; independent of GeNN XORWOW')
        save(out/'import.json',rec);print(json.dumps({k:rec[k] for k in ('tick','canonical_trace_error','canonical_graph_sha256','weights_sha256','neurons_sha256','complete')}),flush=True)
        return rec
    except BaseException as e:
        rec.update(error=repr(e),end=time.time());save(out/'import.json',rec);raise

def main(argv=None):
    """Convert a native checkpoint without advancing its neural state."""
    import argparse
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == '--export':
        if len(argv) != 6:
            raise SystemExit('invalid internal exporter arguments')
        exporter(argv[1], argv[2], argv[3], Spec(**json.loads(argv[4])), int(argv[5]))
        return
    from workload_paths import resolve_retained_path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', required=True, help='native state directory; remote path with --host')
    parser.add_argument('--graph', type=Path, required=True, help='local native graph directory')
    parser.add_argument('--output', type=Path, required=True, help='new reduced-state directory')
    parser.add_argument('--expected-sha256', help='state manifest SHA-256; required for remote input')
    parser.add_argument('--spec', help='JSON object or JSON file containing Spec fields (default: full model)')
    parser.add_argument('--host', help='explicit SSH host for read-only export')
    parser.add_argument('--remote-graph', help='graph path on the SSH host')
    parser.add_argument('--archive-root', type=Path, help='relocated archive/tmp root')
    args = parser.parse_args(argv)
    fields = {}
    if args.spec:
        text = args.spec if args.spec.lstrip().startswith('{') else Path(args.spec).read_text()
        fields = json.loads(text)
        fields = fields.get('spec', fields)
    spec = Spec(**fields)
    graph = resolve_retained_path(args.graph, archive_root=args.archive_root)
    if args.host:
        if not args.expected_sha256 or not args.remote_graph:
            parser.error('--host requires --expected-sha256 and --remote-graph')
        state = args.state
    else:
        state = resolve_retained_path(args.state, archive_root=args.archive_root)
    expected = args.expected_sha256 or sha256(Path(state)/'manifest.json')
    if len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected):
        parser.error('--expected-sha256 must be a lowercase SHA-256 hex digest')
    reduce_checkpoint(state, graph, args.output, spec, expected,
                      host=args.host, remote_graph=args.remote_graph)


if __name__ == '__main__':
    main()
