"""ABI4 Morrison layout with plastic ranges placed in worker MCs.

The canonical ABI3 image remains the source of graph/state/edge identity.  The
layout is deliberately restricted to balanced G32 on a 32-PU/MC system, which
is the target and the existing 64-PU/2-MC functional geometry.
"""
from pathlib import Path
import hashlib
import numpy as np

from morrison_locality import Repacker
from morrison_model import MC_BYTES, NODE_BASE, NODE_BYTES, PARAMETERS


class WorkerLayout:
    layout = 'distant-locality-worker-mc-v1'
    abi = 4

    def __init__(self, images, payload, helper, *, random_seed=None):
        if random_seed is not None and (type(random_seed) is not int or not 0 <= random_seed < 2**64):
            raise ValueError('placement seed must be an unsigned 64-bit integer')
        self.random_seed = random_seed
        if random_seed is not None:
            self.layout = 'distant-random-worker-mc-v1'
        self.locality = Repacker(images, payload, helper)
        self.image = self.locality.image
        self.spec = s = self.image.spec
        self.code = Path(payload).read_bytes()
        self.width = int(self.image.manifest['dispatch_width'])
        if self.width != 32 or s.pu_per_mc != 32:
            raise ValueError('worker layout requires balanced G32 and 32 PUs per MC')
        self.degree = np.load(self.image.directory/'plastic-outdegree.npy').astype(np.int64)
        if np.any(self.degree >= 65536):
            raise ValueError('per-row permutation does not fit uint16')
        self.old_start = np.full(s.mc, 0x10000, np.int64)
        self.old_end = self.old_start.copy()
        self.source_base = np.zeros(s.ne, np.uint32)
        for mc, rows in enumerate(self.locality.plastic):
            for home, source, base, count in rows:
                self.old_start[mc] = min(self.old_start[mc], int(base))
                self.old_end[mc] = max(self.old_end[mc], int(base)+(int(count)+8)//8*64)
                self.source_base[int(source)] = int(base)
        size = ((self.degree + 8)//8*64).astype(np.int64)
        units = (size + 1023)//1024
        counts = units[:, None]//self.width + (np.arange(self.width) < units[:, None] % self.width)
        # The last rank may contain a partial 128-edge block.  Store the exact
        # byte lengths used by mb_home_span, including its final short block.
        self.offset = (np.cumsum(counts, axis=1)-counts)*1024
        self.length = np.maximum(0, np.minimum(counts*1024, size[:, None]-self.offset))
        self.edge_starts = np.minimum(self.offset//8, self.degree[:, None]).astype(np.uint32)
        self.edge_ends = np.minimum((self.offset+self.length)//8, self.degree[:, None]).astype(np.uint32)
        self.workers = np.asarray(self.locality.workers, np.int64)
        self.base = np.zeros((s.ne, self.width), np.uint32)
        cursor = self.old_start.copy()
        for source in range(s.ne):
            home = source % s.pu
            for rank in np.flatnonzero(self.length[source]):
                mc = int(self.workers[home, rank]//s.pu_per_mc)
                self.base[source, rank] = cursor[mc]
                cursor[mc] += self.length[source, rank]
        self.new_end = cursor.copy()
        self.shift = self.new_end - self.old_end
        cursor = np.asarray(self.image.manifest['image_bytes'], dtype=np.int64) + self.shift
        self.directory = np.zeros(s.pu, np.uint32)
        for pu in range(s.pu):
            mc = pu//s.pu_per_mc
            self.directory[pu] = int(cursor[mc])
            cursor[mc] += len(range(pu, s.ne, s.pu))*self.width*8
        self.sizes = cursor
        if np.any(cursor > MC_BYTES) or np.any(cursor % 32):
            raise ValueError('worker image exceeds physical capacity/alignment')
        self.permutation = None
        self.permutation_offsets = np.r_[0, np.cumsum(self.degree, dtype=np.int64)]

    def prepare(self):
        if self.permutation is not None:
            return
        total = int(self.permutation_offsets[-1])
        self.permutation = np.empty(total, np.uint16)
        for source, n in enumerate(self.degree):
            if self.random_seed is None:
                _, _, perm, quotas = self.locality.permutation(source % self.spec.pu,
                                                                int(self.source_base[source]), int(n))
                expected = self.edge_ends[source] - self.edge_starts[source]
                if not np.array_equal(expected.astype(np.int64), np.minimum(quotas, int(n))):
                    raise ValueError(f'locality quota differs from runtime split for source {source}')
            else:
                # Independent streams prevent similarly ordered incoming columns
                # from following the same worker rank in every source row. The
                # fixed range capacities still balance forward work and storage.
                seed = np.random.SeedSequence([self.random_seed, source])
                perm = np.random.Generator(np.random.PCG64(seed)).permutation(int(n))
            lo, hi = self.permutation_offsets[source:source+2]
            self.permutation[lo:hi] = perm

    def ranges(self, mc=None, home_mc=None):
        for source in range(self.spec.ne):
            home = source % self.spec.pu
            if home_mc is not None and home//self.spec.pu_per_mc != home_mc:
                continue
            for rank in np.flatnonzero(self.length[source]):
                target_mc = int(self.workers[home, rank]//self.spec.pu_per_mc)
                if mc is None or target_mc == mc:
                    yield source, int(rank), target_mc

    def addresses(self, source, rank):
        self.prepare()
        start = int(self.edge_starts[source, rank])
        stop = int(self.edge_ends[source, rank])
        lo = int(self.permutation_offsets[source])
        old = self.permutation[lo+start:lo+stop].astype(np.uint32)
        old_w = int(self.source_base[source])//4 + old//128*256 + old%128
        stored = (int(self.degree[source])+8)//8*8
        old_d = old_w + np.where(old < stored//128*128, 128, stored%128)
        slots = int(self.length[source, rank])//8
        i = np.arange(stop-start, dtype=np.uint32)
        new_w = int(self.base[source, rank])//4 + i//128*256 + i%128
        new_d = new_w + np.where(i < slots//128*128, 128, slots%128)
        return old_w, old_d, new_w, new_d

    def _patch_directories(self, mc, data):
        s = self.spec
        for pu in range(mc*s.pu_per_mc, (mc+1)*s.pu_per_mc):
            rows = self.image.rows[pu]
            for field in (5, 6):
                for row in rows:
                    old_base = int(row[field])
                    if not old_base:
                        continue
                    entries = data[(old_base+int(self.shift[mc]))//4:][:s.mc*2].reshape(-1, 2)
                    used = entries[:, 1] != 0
                    for target_mc in np.flatnonzero(used):
                        entries[target_mc, 0] = np.uint32(int(entries[target_mc, 0]) + int(self.shift[target_mc]))

    def _patch_reverse(self, mc, data):
        """Remap every reverse entry to its worker MC and physical address."""
        self.prepare()
        start = int(self.old_start[mc])//4
        remap = np.zeros((int(self.old_end[mc])-int(self.old_start[mc]))//4, np.uint32)
        for source, rank, target_mc in self.ranges(home_mc=mc):
            old_w, _, new_w, _ = self.addresses(source, rank)
            remap[old_w-start] = new_w | np.uint32(target_mc << 28)
        for address, count in self.locality.reverse[mc]:
            words = data[(int(address)+int(self.shift[mc]))//4:][:((int(count)+3)//4*4)*2].reshape(-1, 2)
            valid = words[:, 0] != 0
            if not np.any(valid):
                continue
            mapped = remap[words[valid, 1]//4-start]
            if np.any(mapped == 0):
                raise ValueError('reverse entry does not name a relocated weight')
            words[valid, 0] = (words[valid, 0]&0xfff0ffff) | ((mapped >> 28) << 16)
            words[valid, 1] = (mapped&0x0fffffff)*4

    def initial(self, mc):
        self.prepare()
        original = self.image.memory[mc]
        observed = hashlib.sha256(original).hexdigest()
        if observed != self.image.manifest['image_sha256'][mc]:
            raise ValueError('canonical input image changed')
        old_start, old_end, new_end = map(int, (self.old_start[mc], self.old_end[mc], self.new_end[mc]))
        data = np.zeros(int(self.sizes[mc])//4, '<u4')
        data[:old_start//4] = original[:old_start//4]
        data[new_end//4:new_end//4+len(original)-old_end//4] = original[old_end//4:]
        for source, rank, _ in self.ranges(mc=mc):
            old_w, old_d, new_w, new_d = self.addresses(source, rank)
            source_mc = (source % self.spec.pu)//self.spec.pu_per_mc
            source_memory = self.image.memory[source_mc]
            slots = int(self.length[source, rank])//8
            i = np.arange(slots, dtype=np.uint32)
            all_w = int(self.base[source, rank])//4 + i//128*256 + i%128
            all_d = all_w + np.where(i < slots//128*128, 128, slots%128)
            data[all_d] = 0xffffffff
            data[new_w] = source_memory[old_w]
            data[all_d[:len(old_d)]] = source_memory[old_d]
        self._patch_directories(mc, data)
        self._patch_reverse(mc, data)
        for pu in range(mc*self.spec.pu_per_mc, (mc+1)*self.spec.pu_per_mc):
            bases = self.base[pu::self.spec.pu]
            entries = np.empty((*bases.shape, 2), np.uint32)
            entries[:, :, 0] = ((0x2d00+(np.arange(self.width)%4)*4)<<16) | (pu+1)
            entries[:, :, 1] = bases
            words = entries.ravel()
            offset = int(self.directory[pu])//4
            data[offset:offset+len(words)] = words
        if mc == 0:
            data.view(np.uint8)[16:16+len(self.code)] = np.frombuffer(self.code, np.uint8)
            old_section = int(original[1])
            section = old_section + int(self.shift[0])
            data[1] = section
            for pu in range(self.spec.pu):
                offset = int(original[old_section//4+pu*2])
                params = (section+offset+PARAMETERS)//4
                home_mc = pu//self.spec.pu_per_mc
                data[params+3] = np.uint32(int(data[params+3])+int(self.shift[home_mc]))
                data[params+4] = self.abi
                data[params+6] = np.uint32(int(data[params+6])+int(self.shift[home_mc]))
                data[params+10] = self.directory[pu]
        return data, dict(mc=mc, bytes=data.nbytes, base_sha256=observed,
                          physical_sha256=hashlib.sha256(data).hexdigest(),
                          remaining_bytes=MC_BYTES-data.nbytes)

    def verify_delta(self, mc, delta):
        """Check every changed word against the allowed weight addresses."""
        if delta.dtype != np.dtype('<u4') or delta.nbytes != self.sizes[mc]:
            raise ValueError('invalid physical delta shape')
        scratch = delta.copy()
        changed = 0
        for source, rank, _ in self.ranges(mc=mc):
            _, _, weight, _ = self.addresses(source, rank)
            changed += int(np.count_nonzero(scratch[weight]))
            scratch[weight] = 0
        if mc == 0:
            scratch[0x8000//4:0x8080//4] = 0
        if np.any(scratch):
            raise ValueError('non-weight physical image data changed')
        return changed

    def normalize_small(self, physical, output):
        """Small-fixture adapter for the existing independent arithmetic oracle."""
        canonical = [np.array(x, copy=True) for x in self.image.memory]
        for mc, data in enumerate(physical):
            initial, _ = self.initial(mc)
            self.verify_delta(mc, np.bitwise_xor(data, initial))
            for source, rank, _ in self.ranges(mc=mc):
                old_w, _, new_w, _ = self.addresses(source, rank)
                home_mc = source % self.spec.pu//self.spec.pu_per_mc
                canonical[home_mc][old_w] = data[new_w]
        canonical[0][0x8000//4:0x8080//4] = physical[0][0x8000//4:0x8080//4]
        for mc, data in enumerate(canonical):
            data.tofile(Path(output)/f'dram.{mc}')
