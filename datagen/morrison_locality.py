#!/usr/bin/env python3
"""Reversible locality permutation of ABI3 plastic rows in their source MC.

The immutable original image fixes edge identity and the permutation, even when
transforming final learned weights. No graph or neuron state is regenerated.
"""
from pathlib import Path
import ctypes
import hashlib
import numpy as np

from morrison import topology
from morrison_validate import Image

LAYOUT = 'distant-locality-source-mc-v1'


class Repacker:
    def __init__(self, images, payload, helper):
        self.image = Image(images, hashes=False)
        self.spec = s = self.image.spec
        m = self.image.manifest
        if self.image.abi != 3 or m.get('dispatch_policy') != 'balanced':
            raise ValueError('locality repacker requires balanced ABI3')
        self.width = m['dispatch_width']
        self.override = s.pu//2 if m.get('fixture', {}).get('cross_mc_jobs') else 0
        self.code = Path(payload).read_bytes()
        if len(self.code)+16 > 8192:
            raise ValueError('distant payload exceeds its instruction region')
        self.library = ctypes.CDLL(str(Path(helper).resolve()))
        self.assign = self.library.morrison_locality_assign
        self.assign.argtypes = [ctypes.c_uint32]*3 + [ctypes.c_void_p]*5
        self.assign.restype = ctypes.c_int
        xy = topology(s.pu)
        self.distance = np.abs(xy[:, None]-xy[None, :]).sum(axis=2)
        self.preferences, self.regrets = [], []
        self.workers = []
        for home in range(s.pu):
            stride = s.pu//self.width
            workers = (home%stride + (home//stride+np.arange(self.width))%self.width*stride)^self.override
            costs = self.distance[:, workers]
            preferences = np.argsort(costs, axis=1, kind='stable').astype(np.uint8)
            sorted_costs = np.take_along_axis(costs, preferences, axis=1)
            regret = sorted_costs[:, 1]-sorted_costs[:, 0] if self.width>1 else np.zeros(s.pu)
            self.workers.append(workers)
            self.preferences.append(np.ascontiguousarray(preferences))
            self.regrets.append(np.ascontiguousarray(regret, dtype=np.uint16))
        # Gather all reverse column intervals once. Their entries/row order stay
        # fixed; only each entry's MC-local weight address changes.
        self.reverse = [[] for _ in range(s.mc)]
        self.plastic = [[] for _ in range(s.mc)]
        for home, rows in enumerate(self.image.rows):
            mc = home//s.pu_per_mc
            for row in rows:
                if int(row[2]) >= s.ne:
                    continue
                n, base = int(row[4]), int(row[3])
                self.plastic[mc].append((home, int(row[2]), base, n))
                for target_mc, (address, count) in enumerate(self.image.directory_entries(mc, int(row[6]))):
                    if count:
                        self.reverse[target_mc].append((int(address), int(count)))
        for mc in range(s.mc):
            self.reverse[mc].sort()
            self.plastic[mc].sort(key=lambda r:r[2])
            for (a,n),(b,_) in zip(self.reverse[mc],self.reverse[mc][1:]):
                if a+(n+3)//4*32 != b:
                    raise ValueError('reverse rows not a contiguous scatter region')

    def permutation(self, home, base, n):
        mc = home//self.spec.pu_per_mc
        original = self.image.memory[mc]
        i = np.arange(n, dtype=np.uint32)
        weights = base//4+i//128*256+i%128
        slots = (n+8)//8*8
        destinations = weights+np.where(i<slots//128*128, 128, slots%128).astype(np.uint32)
        packed = np.ascontiguousarray(original[destinations])
        size = slots*8
        units = (size+1023)//1024
        chunks = np.full(self.width, units//self.width, np.uint32)
        chunks[:units%self.width] += 1
        ends = np.minimum(np.cumsum(chunks, dtype=np.uint32)*128, n)
        quotas = np.diff(np.r_[np.uint32(0),ends]).astype(np.uint32)
        perm = np.empty(n, np.uint32)
        pref, regret = self.preferences[home], self.regrets[home]
        rc = self.assign(n, self.spec.pu, self.width, packed.ctypes.data,
                         pref.ctypes.data, regret.ctypes.data, quotas.ctypes.data, perm.ctypes.data)
        if rc or not np.array_equal(np.sort(perm), i):
            raise ValueError(f'non-bijective locality permutation ({rc})')
        return weights, destinations, perm, quotas

    def transform(self, mc, data, inverse=False, check_initial=False):
        """Transform a writable uint32 image in place, with immutable checks.

        inverse=True restores canonical ordering for lossless archive storage;
        transform(inverse=False) reconstructs its full physical image again.
        """
        original = self.image.memory[mc]
        if data.dtype != np.dtype('<u4') or data.shape != original.shape or not data.flags.writeable:
            raise ValueError('invalid writable image buffer')
        if check_initial and not np.array_equal(data, original):
            raise ValueError('input differs from immutable base')
        rows = self.plastic[mc]
        low = rows[0][2]//4 if rows else 0
        high = (rows[-1][2]+(rows[-1][3]+8)//8*64)//4 if rows else 0
        remap = np.full(high-low, 0xffffffff, np.uint32)
        hops = edges = 0
        for home, source, base, n in rows:
            w, d, perm, quotas = self.permutation(home, base, n)
            # Explicit immutable target/sentinel/padding verification prevents
            # normalization from masking an erroneous runtime write.
            expected = original[d][perm] if inverse else original[d]
            if not np.array_equal(data[d], expected):
                raise ValueError(f'changed plastic destinations: source {source}')
            slots = (n+8)//8*8
            padding = np.arange(n, slots, dtype=np.uint32)
            pw = base//4+padding//128*256+padding%128
            pd = pw+np.where(padding<slots//128*128,128,slots%128).astype(np.uint32)
            if np.any(data[pw]) or np.any(data[pd] != 0xffffffff):
                raise ValueError(f'changed sentinel/padding: source {source}')
            values = data[w].copy()
            if inverse:
                data[w[perm]] = values
                data[d] = original[d]
            else:
                data[w] = values[perm]
                data[d] = original[d][perm]
            remap[w[perm]-low] = w*4
            targets = (original[d][perm]&65535)-1
            owners = np.repeat(self.workers[home], quotas)
            hops += int(self.distance[owners, targets].sum(dtype=np.uint64))
            edges += n
        reverse_count = 0
        if self.reverse[mc]:
            first = self.reverse[mc][0][0]//4
            a,n = self.reverse[mc][-1]
            end = (a+(n+3)//4*32)//4
            for start in range(first,end,1<<20):
                stop=min(end,start+(1<<20))
                before=original[start:stop].reshape(-1,2)
                after=data[start:stop].reshape(-1,2)
                if not np.array_equal(before[:,0],after[:,0]):
                    raise ValueError('reverse source/trace identity changed')
                real=before[:,0]!=0
                if np.any(after[~real]) or np.any(before[real,1]%4):
                    raise ValueError('reverse padding/alignment changed')
                addresses=before[real,1]//4
                if np.any(addresses<low) or np.any(addresses>=high):
                    raise ValueError('reverse pointer outside plastic data')
                mapped=remap[addresses-low]
                if np.any(mapped==0xffffffff):
                    raise ValueError('reverse pointer does not address a weight')
                expected=mapped if inverse else before[real,1]
                if not np.array_equal(after[real,1],expected):
                    raise ValueError('reverse weight pointer changed')
                after[real,1]=before[real,1] if inverse else mapped
                reverse_count += int(real.sum())
        if reverse_count != edges:
            raise ValueError('forward/reverse edge count mismatch')
        if mc==0:
            region=np.array(original[4:2048], copy=True)
            region.view(np.uint8)[:len(self.code)]=np.frombuffer(self.code,np.uint8)
            expected=region if inverse else original[4:2048]
            if not np.array_equal(data[4:2048],expected):
                raise ValueError('payload region changed')
            data[4:2048]=original[4:2048] if inverse else region
        return dict(mc=mc,edges=edges,reverse_entries=reverse_count,plastic_delivery_hops=hops,
                    image_bytes=data.nbytes,inverse=inverse)

    def initial(self, mc):
        data=np.array(self.image.memory[mc],copy=True)
        h=hashlib.sha256(data).hexdigest()
        if h != self.image.manifest['image_sha256'][mc]:
            raise ValueError('immutable input image hash mismatch')
        result=self.transform(mc,data,check_initial=True)
        result['base_sha256']=h
        result['physical_sha256']=hashlib.sha256(data).hexdigest()
        return data,result
