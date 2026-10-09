#include "soft_components.h"
#include <iostream>

static void require(bool value,const char *why) {
  if (!value) throw std::runtime_error(why);
}
using TestRouter=Router<std::function<size_t(uint16_t)>,8>;
static std::unique_ptr<Flit> packet(unsigned priority,unsigned destination) {
  return std::make_unique<Flit>(Flit{1,uint16_t(destination),uint16_t(priority<<10),{}});
}
static bool inject(TestRouter &router,unsigned priority,unsigned destination) {
  if (!router.prepareEnq(0,priority)) return false;
  router.place(0,packet(priority,destination));
  router.step({});
  return true;
}
static void consume(TestRouter &router,unsigned output) {
  auto token=router.peekToken(output);require(bool(token),"missing output candidate");
  router.take(*token);
  TestRouter::OutputTokens accepted{};accepted[output]=token;router.step(accepted);
}
int main() {
  const auto route=[](uint16_t dst)->size_t{return dst-1;};
  for (unsigned multiplier:{1,2}) {
    for (unsigned priority=0;priority<4;++priority) {
      TestRouter router(route,1,2,multiplier);
      const unsigned capacity=8-priority*multiplier;
      for(unsigned i=0;i<capacity;++i)require(inject(router,priority,1),"admitted too few");
      require(!inject(router,priority,1),"admitted past reservation");
      for(unsigned i=capacity;i<8;++i)require(inject(router,0,2),"reserved P0 space unavailable");
      require(!inject(router,0,2),"accepted into full queue");
      // Highest priority passes a queue of lower-priority packets.
      if(priority) {
        require(!router.peekToken(0)&&router.peekToken(1),"candidate priority changed");
        for(unsigned i=capacity;i<8;++i)consume(router,1);
      }
      for(unsigned i=0;i<capacity;++i)consume(router,0);
      require(router.totalFlits()==0,"queue did not drain");
    }
    TestRouter blocked(route,1,2,multiplier);
    require(inject(blocked,1,1)&&inject(blocked,1,2),"setup");
    require(blocked.peekToken(0)&&!blocked.peekToken(1),"routed hidden alternate destination");
    blocked.step({}); // output0 does not accept; output1 cannot see behind it.
    require(!blocked.peekToken(1),"blocked candidate was bypassed");
    consume(blocked,0);require(bool(blocked.peekToken(1)),"alternate did not become visible");
    consume(blocked,1);
    std::cout<<"PASS multiplier="<<multiplier<<" thresholds/priority/reserved space/candidate-before-route\n";
  }
  TestRouter defaults(route,1,2);
  for(unsigned i=0;i<5;++i)require(inject(defaults,3,1),"default no longer multiplier1");
  require(!inject(defaults,3,1),"default reservation changed");
}
