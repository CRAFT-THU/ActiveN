#include "soft_mem.h"
#include <iostream>
#include <cstring>
#include <map>
#include <set>
using namespace soft_mem;
static void require(bool v, const char *why) { if (!v) throw std::runtime_error(why); }
struct TestDram : DRAMIf {
  explicit TestDram(size_t n=256,bool merge=true,AllocationMode mode=AllocationMode::FirstFree)
      :DRAMIf(1,1,2,1,17,{n,merge,mode}){}
};
static Flit wr(uint32_t addr,uint32_t value,unsigned size=2,bool receipt=false) {
  return {1,0x8001,uint16_t(receipt?1:2),{addr,uint32_t(size<<16),0,value}};
}
static Flit rd(uint32_t addr,uint16_t token=0) {return {1,0x8001,0,{addr,uint32_t(5<<16)|token,0x12345678,0xabcdef01}};}
static bool tick(TestDram &d,const Flit *f=nullptr,bool accept=false,std::optional<GlobalMemResp> resp={}) {
  d.prepareExternal(accept);
  auto port=d.nocAcceptMultiple([&](size_t)->const Flit*{return f;});
  bool injected=false;RingResp ring{};uint16_t mask=0xffff;
  MemBusIn mem{accept,resp};
  std::optional<std::pair<std::reference_wrapper<const Flit>,uint8_t>> input;
  if(port) input={{std::cref(*f),*port}};
  d.step({&ring,false,false,true,&injected},mem,input,std::span<const uint16_t>(&mask,1),false);
  return port.has_value();
}
static std::array<uint8_t,8192> memory;
static GlobalMemResp apply(const GlobalMemReq &r) {
  const auto base=r.addr&~uint32_t(MEM_BUS_WIDTH_B-1);
  require(base+MEM_BUS_WIDTH_B<=memory.size(),"test address");
  GlobalMemResp out{r.id,{}};
  for(unsigned i=0;i<MEM_BUS_WIDTH_B;++i)out.data[i]=memory[base+i];
  if(r.write) for(unsigned i=0;i<(1u<<r.size);++i) {
    const unsigned lane=(r.addr-base)+i;
    if((r.wbe>>lane)&1)memory[base+lane]=r.wdata[lane];
  }
  return out;
}
static void drain(TestDram &d) {
  for(unsigned limit=0;limit<10000 && d.mshrInflightCount();++limit) {
    auto r=d.peekMem();
    if(r) tick(d,nullptr,true,apply(*r)); else tick(d);
  }
  require(d.mshrInflightCount()==0 && d.mshrUnissuedCount()==0,"drain leaked resources");
}
int main() {
  static_assert(MEM_BUS_WIDTH_B==32);
  for(auto mode:{AllocationMode::FirstFree,AllocationMode::Sequential})
  for(size_t n:{33u,256u,257u,384u,512u,513u}) {
    memory.fill(0xa5);
    TestDram d(n,true,mode);
    auto a=wr(0x44,0x11223344),b=wr(0x5c,0x55667788),c=wr(0x45,0xbbbbbbbb,0),h=wr(0x4a,0xa1b2a1b2,1);
    require(tick(d,&a)&&tick(d,&b)&&tick(d,&c)&&tick(d,&h),"merge admission");
    require(d.mshrInflightCount()==1 && d.cycleEvents().mergedWrite,"merge did not reuse slot");
    auto r=d.peekMem();require(r&&r->addr==0x40&&r->size==5,"merged line shape");
    drain(d);
    require(memory[0x44]==0x44&&memory[0x45]==0xbb&&memory[0x46]==0x22&&memory[0x47]==0x11,"overlap byte order");
    require(memory[0x4a]==0xb2&&memory[0x4b]==0xa1&&memory[0x5c]==0x88&&memory[0x5f]==0x55,"disjoint data");
    for(unsigned i=0x40;i<0x60;++i)if(!(i>=0x44&&i<=0x47)&&!(i>=0x4a&&i<=0x4b)&&i<0x5c)require(memory[i]==0xa5,"untouched byte overwritten");

    TestDram order(n,true,mode);memory.fill(0);
    auto w0=wr(0x40,1),read=rd(0x40),w1=wr(0x44,2),w2=wr(0x48,3);
    tick(order,&w0);tick(order,&read);tick(order,&w1);tick(order,&w2);
    require(order.mshrUnissuedCount()==3,"read did not block older merge");
    auto first=order.peekMem();tick(order,nullptr,true,apply(*first));
    auto second=order.peekMem();require(second&&!second->write,"read reordered");
    auto data=apply(*second);require(data.data[0]==1&&data.data[4]==0&&data.data[8]==0,"read saw future merged write");
    tick(order,nullptr,true,data);drain(order);require(memory[0x44]==2&&memory[0x48]==3,"later values missing");

    TestDram edge(n,true,mode);tick(edge,&w0);auto outgoing=edge.peekMem();
    require(tick(edge,&w1,true,apply(*outgoing)),"same-edge incoming admission");
    require(!edge.cycleEvents().mergedWrite && edge.mshrUnissuedCount()==1,"merged into issued head");drain(edge);

    TestDram receipt(n,true,mode);auto normal=wr(0x40,4,2,true);tick(receipt,&normal);tick(receipt,&w1);
    require(receipt.mshrUnissuedCount()==2,"merged receipt-bearing store");drain(receipt);
    TestDram receipt2(n,true,mode);tick(receipt2,&w0);tick(receipt2,&normal);
    require(receipt2.mshrUnissuedCount()==2,"absorbed incoming receipt");drain(receipt2);

    TestDram full(n,true,mode);
    for(unsigned i=0;i<n;++i){auto f=wr(i*32,100+i);require(tick(full,&f),"pool filled early");}
    auto match=wr(4,999);
    require(!tick(full,&match)&&!full.cycleEvents().mergedWrite,"full pool admitted merge");
    full.prepareExternal(true);require(!full.nocAcceptMultiple([&](size_t)->const Flit*{return &match;}),"full pool matched issuing head");
    // Drain only to exercise tag lifecycle; separate memory oracle covered byte correctness.
    for(unsigned i=0;i<n;++i){auto q=full.peekMem();require(bool(q),"full pool issue");tick(full,nullptr,true,GlobalMemResp{q->id,{}});}
    require(full.mshrInflightCount()==0,"posted write drain leak");
    Flit idle{1,0x8001,0xf0,{7,0,0,0}};require(tick(full,&idle),"idle registration");tick(full);tick(full);
    require(full.peekNoc()&&full.peekNoc()->data[0]==0,"idle notification missing after merged stores");

    // Issue the entire pool before any response. IDs are the MSHR indices,
    // including values above255; no independent external-tag window remains.
    TestDram ids(n,false,mode);
    for(unsigned i=0;i<n;++i) ids.mshrAllocCommit(i,MSHR::fromScalarFlit(rd(i*32,uint16_t(0x9000+i))));
    std::vector<uint16_t> outstanding;
    for(unsigned i=0;i<n;++i) {
      auto q=ids.peekMem();require(q&&q->addr==i*32&&q->id==i,"direct issue ID/address");
      outstanding.push_back(q->id);tick(ids,nullptr,true);
    }
    require(!ids.peekMem()&&ids.externalIdCount()==n,"full pool did not issue without responses");
    tick(ids,nullptr,true);
    require(ids.externalIdCount()==n,"ghost issue without presentation");
    for(auto it=outstanding.rbegin();it!=outstanding.rend();++it) {
      MemLine bytes{};uint32_t value=0xbe000000u+*it;std::memcpy(bytes.data(),&value,4);
      ids.mshrRespAccept({*it,bytes});
    }
    std::set<unsigned> seen;
    while(auto slot=ids.mshrReturnSlot(false)) {
      auto response=ids.mshrUnicast(*slot);uint32_t got;std::memcpy(&got,response.data.data(),4);
      require(got==0xbe000000u+*slot && response.tag==0x9000+*slot,"direct response reached wrong MSHR");
      require(response.ident==0x5678 && response.extra0==0x1234 && response.extra1==0xabcdef01,"response metadata changed");
      require(seen.insert(*slot).second,"duplicate return");ids.mshrReturnCommit(*slot,false);
    }
    require(seen.size()==n&&ids.externalIdCount()==0&&ids.mshrInflightCount()==0,"large pool/arbiter wrap leak");
    if(n>256)require(*seen.rbegin()>255,"high internal slots untested");
    std::cout<<"PASS mode="<<allocationName(mode)<<" slots="<<n<<" outstanding="<<outstanding.size()<<" byte masks/order/full rejection/same edge/receipts/idle/direct IDs/response reorder/wrap\n";
  }
}
