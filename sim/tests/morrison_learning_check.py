#!/usr/bin/env python3
"""Check settled-run learning and transport against the observed spike history.

This does not certify the membrane trajectory: spikes are observations, while
trace evolution and all affected weights are reconstructed independently.
"""
import argparse
import csv
import json
from pathlib import Path
import struct
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'datagen'))
from morrison_model import F, MAGIC, constants
from morrison_validate import Image


def check(images, output):
    image, output = Image(images), Path(output)
    spec = image.spec
    if not image.bulk:
        raise ValueError('this learning replay requires whole-row ABI2 images')
    state = np.load(Path(images)/'neurons.npy')
    x, y = state[:spec.ne,3].view(F).copy(), state[:spec.ne,4].view(F).copy()
    c = constants()
    changed = [dict() for _ in range(spec.mc)]
    activity = list(csv.DictReader((output/'morrison-activity.csv').open()))
    steps = image.manifest['warmup']+image.manifest['timed']
    if len(activity)!=steps: raise ValueError('incomplete activity window')
    def weights(mc, addresses):
        original = image.memory[mc][addresses.astype(np.uint64)//4].view(F)
        return np.asarray([changed[mc].get(int(a),w) for a,w in zip(addresses,original)],F)
    forward_total = reverse_total = 0
    with (output/'morrison-spikes.bin').open('rb') as stream:
        if struct.unpack('<4I',stream.read(16)) != (MAGIC,spec.ne,spec.ni,spec.pu):
            raise ValueError('bad spike header')
        for step, observed in enumerate(activity,1):
            header=stream.read(8)
            if len(header)!=8: raise ValueError('missing spike frame')
            number,count=struct.unpack('<2I',header)
            ids=np.frombuffer(stream.read(count*4),dtype='<u4')
            if number!=step or int(observed['step'])!=step or len(ids)!=count or np.any(ids>=spec.n) or len(np.unique(ids))!=count:
                raise ValueError('bad spike identity/window')
            if int((ids<spec.ne).sum())!=int(observed['e_spikes']) or int((ids>=spec.ne).sum())!=int(observed['i_spikes']):
                raise ValueError('spike/activity disagreement')
            fired=np.zeros(spec.n,bool);fired[ids]=True
            x=F(x*c['decay_pre']);y=F(y*c['decay_post'])
            forward=reverse=0
            for source in ids:
                source=int(source);pu=source%spec.pu;mc=pu//spec.pu_per_mc
                row=image.rows[pu][source//spec.pu]
                post,_,address=image.forward(pu,row)
                forward+=len(post)
                quiet=~fired[post]
                address=address[quiet];w=weights(mc,address)
                new=np.maximum(F(0),F(w-F(w*y[post[quiet]])))
                changed[mc].update((int(a),v) for a,v in zip(address,new))
                forward+=int(image.directory_entries(mc,int(row[5]))[:,1].sum(dtype=np.uint64))
            for post in ids[ids<spec.ne]:
                post=int(post);pu=post%spec.pu
                row=image.rows[pu][post//spec.pu]
                for mc,(base,n) in enumerate(image.directory_entries(pu//spec.pu_per_mc,int(row[6]))):
                    entries=image.read(mc,int(base),int(n)*2).reshape(-1,2)
                    packed,address=entries[:,0],entries[:,1]
                    pre=(((packed>>16)&0xfff0)-256)//32*spec.pu+(packed&65535)-1
                    w=weights(mc,address);active=fired[pre]
                    w[active]=np.maximum(F(0),F(w[active]-F(w[active]*y[post])))
                    w=F(w+F(x[pre]*np.power(w.astype(np.float64),2./5.).astype(F)))
                    changed[mc].update((int(a),v) for a,v in zip(address,w))
                    reverse+=int(n)
            exc=ids[ids<spec.ne]
            x[exc]=F(x[exc]+c['pot']);y[exc]=F(y[exc]+c['dep'])
            if (forward,reverse)!=(int(observed['forward_deliveries']),int(observed['reverse_visits'])):
                raise ValueError(f'step{step}: transport count differs from fired source rows')
            forward_total+=forward;reverse_total+=reverse
        if stream.read(1):raise ValueError('trailing spike frames')
    peak=0.;max_ulp=0
    for mc,changes in enumerate(changed):
        path=output/f'dram.{mc}'
        if path.stat().st_size!=image.manifest['image_bytes'][mc]:raise ValueError('final image extent differs')
        final=np.memmap(path,mode='r',dtype='<u4')
        address=np.asarray(list(changes),np.uint64)
        expected=np.asarray(list(changes.values()),F)
        actual=final[address//4].view(F)
        if not np.allclose(actual,expected,rtol=4e-7,atol=2e-5):
            i=int(np.argmax(np.abs(actual-expected)))
            raise ValueError(f'MC{mc} weight at{address[i]}: actual{actual[i]} expected{expected[i]}')
        peak=max(peak,float(np.max(np.abs(actual-expected),initial=0)))
        if len(actual): max_ulp=max(max_ulp,int(np.max(np.abs(actual.view(np.uint32).astype(np.int64)-expected.view(np.uint32).astype(np.int64)))))
        allowed=set(changes)
        if mc==0:
            for i in range(32):
                a=0x8000+i*4
                if final[a//4]!=0x19260000+(i&~7)*32+(i&7):raise ValueError('boot test word changed')
                allowed.add(a)
        for start in range(0,len(final),1<<20):
            diff=(np.flatnonzero(image.memory[mc][start:start+(1<<20)]!=final[start:start+(1<<20)])+start)*4
            if any(int(a) not in allowed for a in diff):raise ValueError(f'unexpected initialized-memory write in MC{mc}')
    return dict(steps=steps,conditioned_on_observed_spikes=True,membrane_trajectory_checked=False,
                forward_deliveries=forward_total,reverse_visits=reverse_total,
                touched_weights_checked=sum(map(len,changed)),weight_max_absolute_error=peak,
                weight_max_ulp_difference=max_ulp,weight_units='stored EPSC coefficient',
                other_initialized_image_words_unchanged=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('images',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    print(json.dumps(check(a.images,a.output),indent=2))
