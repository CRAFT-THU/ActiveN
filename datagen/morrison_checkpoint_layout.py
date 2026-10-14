"""Selected physical state over the hash-verified, unchanged ABI3 geometry."""
from pathlib import Path
import hashlib,json,sys
import numpy as np
from workload_paths import resolve_retained_path
from morrison_worker import WorkerLayout
from morrison_model import NODE_BASE,constants
from morrison_graph import sha256

class CheckpointWorkerLayout(WorkerLayout):
    def __init__(self,images,payload,helper,reduced,*,random_seed,graph_path=None,archive_root=None):
        super().__init__(images,payload,helper,random_seed=random_seed)
        self.reduced=Path(reduced);self.import_record=json.loads((self.reduced/'import.json').read_text())
        r=self.import_record
        assert r['complete'] and self.image.manifest['graph_sha256']==r['canonical_graph_sha256']
        assert self.image.manifest['spec']==r['spec']
        assert sha256(self.reduced/'ee_g.npy')==r['weights_sha256']
        assert sha256(self.reduced/'neurons.npy')==r['neurons_sha256']
        assert sha256(self.reduced/'state-manifest.json')==r['expected']
        self.checkpoint_weights=np.load(self.reduced/'ee_g.npy',mmap_mode='r')
        self.checkpoint_neurons=np.load(self.reduced/'neurons.npy',mmap_mode='r')
        assert self.checkpoint_weights.dtype==np.dtype('<f4') and self.checkpoint_weights.shape==(self.spec.ne*self.spec.ce,)
        assert self.checkpoint_neurons.dtype==np.dtype('<u4') and self.checkpoint_neurons.shape==(self.spec.n,8)
        native_graph=resolve_retained_path(graph_path or r['graph_path'],archive_root=archive_root)
        assert sha256(native_graph/'manifest.json')==r['graph_manifest_sha256']
        rows=np.load(native_graph/'ee_rowLength.npy',mmap_mode='r')
        assert np.array_equal(rows,self.degree)
        assert sha256(self.image.directory/'plastic-outdegree.npy')==json.loads((self.reduced/'graph-manifest.json').read_text())['arrays']['ee_rowLength']['sha256']
        self.epsc=float(constants()['epsc'])

    def initial(self,mc):
        data,rec=super().initial(mc);count=0
        for source,rank,_ in self.ranges(mc=mc):
            start=int(self.edge_starts[source,rank]);stop=int(self.edge_ends[source,rank]);lo=int(self.permutation_offsets[source])
            slots=lo+self.permutation[lo+start:lo+stop].astype(np.int64)
            weight=(self.checkpoint_weights[slots].astype(np.float64)*self.epsc).astype(np.float32)
            assert np.all(np.isfinite(weight)) and np.all(weight>=0)
            _,_,address,_=self.addresses(source,rank);data[address]=weight.view('<u4');count+=len(weight)
        if mc==0:
            section=int(data[1])
            for pu in range(self.spec.pu):
                offset=int(data[section//4+pu*2]);base=(section+offset+NODE_BASE)//4
                state=self.checkpoint_neurons[pu::self.spec.pu].reshape(-1)
                data[base:base+len(state)]=state
        rec.update(geometry_sha256=rec['base_sha256'],checkpoint_manifest_sha256=self.import_record['expected'],compact_neurons_sha256=self.import_record['neurons_sha256'],canonical_weights_sha256=self.import_record['weights_sha256'],overlaid_weights=count,physical_sha256=hashlib.sha256(data).hexdigest())
        if mc==0:
            raw=data.view(np.uint8);code=raw[16:16+len(self.code)].copy();raw[16:16+len(self.code)]=0
            rec['noncode_physical_sha256']=hashlib.sha256(data).hexdigest();raw[16:16+len(self.code)]=code
        else:rec['noncode_physical_sha256']=rec['physical_sha256']
        return data,rec
