"""Independent physical decoder for small ABI4 layout fixtures.

Does not import the relocation generator or its permutation. Verify graph
multisets, every reverse edge, local forward storage, sentinels and directory
destinations directly from the emitted words.
"""
from collections import Counter
from pathlib import Path
import hashlib,json,sys
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'datagen'))
from morrison_validate import Image


def check(canonical, physical):
    old=Image(canonical);s=old.spec;physical=Path(physical)
    m=json.loads((physical/'manifest.json').read_text())
    assert m['format']=='activen-morrison-image-v4' and s.pu_per_mc==32
    memory=[np.memmap(physical/f'dram.{mc}',dtype='<u4',mode='r') for mc in range(s.mc)]
    for mc,a in enumerate(memory):
        assert a.nbytes==m['image_bytes'][mc] and a.nbytes<=1<<30
        assert hashlib.sha256(a).hexdigest()==m['image_sha256'][mc]
    def read(mc,address,count):
        assert address%4==0 and address>=0 and address//4+count<=len(memory[mc])
        return memory[mc][address//4:address//4+count]
    boot=read(0,0,4);assert tuple(boot[[0,2,3]])==(0x0100006f,s.mc,s.pu)
    section=int(boot[1]);spms=[];edges={};reverse=[];static=0;sentinels=0
    override=s.pu//2 if old.manifest.get('fixture',{}).get('cross_mc_jobs') else 0
    width=32;stride=s.pu//width
    for pu in range(s.pu):
        off,size=map(int,read(0,section+pu*8,2));assert size==0x3f00
        spm=read(0,section+off,size//4);spms.append(spm);params=spm[0x2e00//4:0x2e00//4+11]
        assert params[4]==4 and params[5]==32 and params[10]%32==0
        assert np.array_equal(spm[:64],old.spms[pu][:64])
        assert np.array_equal(spm[64:64+int(spm[4])*8],old.spms[pu][64:64+int(spm[4])*8])
        assert np.array_equal(spm[0x2ea0//4:0x3214//4],old.spms[pu][0x2ea0//4:0x3214//4])
        home_mc=pu//32
        for slot,row in enumerate(old.rows[pu]):
            source=int(row[2])
            if source<s.ne:
                n=int(row[4]);length=(n+8)//8*64;units=(length+1023)//1024
                directory=read(home_mc,int(params[10])+slot*32*8,64).reshape(32,2)
                observed=[];logical=0
                for rank in range(width):
                    assert int(directory[rank,0])==((0x2d00+4*(rank%4))<<16)|(pu+1)
                    chunks=units//width+int(rank<units%width)
                    size=max(0,min(chunks*1024,length-logical))
                    worker=(pu%stride+((pu//stride+rank)%width)*stride)^override
                    mc=worker//32;base=int(directory[rank,1])
                    if not size:
                        assert base==0
                        continue
                    assert base%32==0 and base>=0x10000
                    for byte in range(0,size,1024):
                        block=min(1024,size-byte);slots=block//8
                        a=read(mc,base+byte,block//4);weights=a[:slots];targets=a[slots:]
                        count=max(0,min(slots,n-(logical+byte)//8))
                        assert np.all(weights[count:]==0) and np.all(targets[count:]==0xffffffff)
                        sentinels+=slots-count
                        for index in range(count):
                            target=int(targets[index]);dest=(target&65535)-1;offset=target>>16
                            assert 0<=dest<s.pu and offset>=276 and (offset-276)%32==0
                            post=(offset-276)//32*s.pu+dest
                            assert post<s.ne and post!=source
                            key=(mc,base+byte+index*4);assert key not in edges
                            edges[key]=(source,post,int(weights[index]));observed.append((post,int(weights[index])))
                    logical+=size
                assert logical==length
                post,weight,_=old.forward(pu,row)
                assert Counter(observed)==Counter(zip(map(int,post),map(int,weight.view(np.uint32))))
            for field,pbase in ((5,int(params[3])),(6,int(params[6]))):
                if field==6 and source>=s.ne:continue
                entries=read(home_mc,pbase+slot*int(params[7]),s.mc*2).reshape(s.mc,2)
                before=old.directory_entries(home_mc,int(row[field]))
                assert np.array_equal(entries[:,1],before[:,1])
                for mc,(address,count) in enumerate(entries):
                    address,count=int(address),int(count)
                    raw=read(mc,address,count*2).reshape(count,2)
                    if field==5:
                        expected=old.read(mc,int(before[mc,0]),count*2).reshape(count,2)
                        assert np.array_equal(raw,expected);static+=count
                    else:
                        for packed,weight_address in raw:
                            packed=int(packed);dest=(packed&65535)-1;offset=packed>>16
                            assert 0<=dest<s.pu and dest//32==mc
                            pre=((offset&0xfff0)-256)//32*s.pu+dest
                            assert (offset&0xfff0)>=256 and ((offset&0xfff0)-256)%32==0
                            reverse.append(((offset&15,int(weight_address)),pre,source))
    seen=set();remote=0
    for key,pre,post in reverse:
        assert key in edges and key not in seen
        assert edges[key][:2]==(pre,post);seen.add(key)
        remote+=int(key[0]!=(pre%s.pu)//32)
    assert len(seen)==len(edges)==s.ne*s.ce
    assert static==s.n*(s.ce+s.ci)-len(edges)
    return dict(plastic_edges=len(edges),reverse_edges=len(seen),static_edges=static,
                padding_slots=sentinels,home_reverse_remote_edges=remote,
                all_forward_plastic_ranges_local=True,graph_and_weight_multisets_exact=True,
                reverse_edge_bijection=True,all_neuron_state_exact=True)


if __name__=='__main__':
    print(json.dumps(check(*map(Path,sys.argv[1:])),indent=2))
