// Stable capacitated locality assignment for the offline row repacker.
// All arrays are caller-owned. permutation[newOrdinal] = oldOrdinal, retaining
// original order within each worker slice, including parallel-edge identity.
#include <array>
#include <cstdint>
#include <vector>

extern "C" int morrison_locality_assign(uint32_t count,uint32_t pus,uint32_t workers,
    const uint32_t *destinations,const uint8_t *preferences,const uint16_t *regrets,
    const uint32_t *capacities,uint32_t *permutation) {
  if (!workers || workers>32 || !pus) return 1;
  std::array<uint32_t,32> remaining{},cursor{};
  uint64_t sum=0;
  for (unsigned j=0;j<workers;++j) {remaining[j]=capacities[j];cursor[j]=sum;sum+=capacities[j];}
  if (sum!=count) return 2;
  unsigned maximum=0;
  for (unsigned p=0;p<pus;++p) if (regrets[p]>maximum) maximum=regrets[p];
  std::vector<uint32_t> buckets(maximum+1),starts(maximum+1),order(count);
  std::vector<uint8_t> chosen(count);
  for (unsigned i=0;i<count;++i) {
    unsigned p=destinations[i]&65535;
    if (!p || p>pus) return 3;
    ++buckets[regrets[p-1]];
  }
  uint32_t offset=0;
  for (int r=maximum;r>=0;--r) {starts[r]=offset;offset+=buckets[r];}
  for (unsigned i=0;i<count;++i) order[starts[regrets[(destinations[i]&65535)-1]]++]=i;
  for (unsigned i:order) {
    unsigned p=(destinations[i]&65535)-1,j=0;
    for (;j<workers;++j) {
      unsigned worker=preferences[p*workers+j];
      if (worker>=workers) return 4;
      if (remaining[worker]) {chosen[i]=worker;--remaining[worker];break;}
    }
    if (j==workers) return 5;
  }
  for (unsigned i=0;i<count;++i) permutation[cursor[chosen[i]]++]=i;
  return 0;
}
