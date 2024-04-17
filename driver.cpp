#include "mvc_CODE/definitions.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <chrono>
#include <iomanip>

using namespace std;

const size_t NCNT = 82469;

int main(int argc, char **argv) {
  ios::sync_with_stdio(false);
  allocateMem();
  initialize();
  initializeSparse();

  ifstream fis(argv[1]);
  fis.seekg(0, std::ios::end);
  size_t size = fis.tellg();
  uint32_t *buf = new uint32_t[size / 4];
  fis.seekg(0);
  assert(size == 4 * (1 + NCNT * 3 + NCNT * maxRowLengthsyn * 2));
  fis.read((char *) buf, size);

  assert(buf[0] == NCNT);
  float *state_ptr = (float *)(buf + 1);
  float *input_ptr = (float *)(buf + NCNT + 1);
  uint32_t *len_ptr = buf + NCNT * 2 + 1;
  memcpy(Vlifs, state_ptr, 4 * NCNT);
  memcpy(inSynsyn, input_ptr, 4 * NCNT);
  memcpy(rowLengthsyn, len_ptr, 4 * NCNT);

  cout<<"Neuron data loaded"<<endl;

  uint32_t *ind_ptr = buf + NCNT * 3 + 1;
  float *w_ptr = (float *)(buf + NCNT * 3 + NCNT * maxRowLengthsyn + 1);

  memcpy(indsyn, ind_ptr, 4 * NCNT * maxRowLengthsyn);
  memcpy(gsyn, w_ptr, 4 * NCNT * maxRowLengthsyn);

  for(int i = 0; i < NCNT; ++i) {
    inSynsyn[i] *= 1 / (1 - 9.04837418035959518e-01f);
    assert(rowLengthsyn[i] <= maxRowLengthsyn);
  }

  for(int i = 0; i < NCNT * maxRowLengthsyn; ++i) {
    gsyn[i] *= 1 / (1 - 9.04837418035959518e-01f);
  }

  for(int i = 0; i < 10; ++i) cout<<Vlifs[i]<<endl;

  cout<<"Synapse data loaded"<<endl;
  pushlifsStateToDevice();
  pushsynStateToDevice();
  pushsynConnectivityToDevice();

  auto begin = std::chrono::high_resolution_clock::now();
  int steps = 0;

  while(t < 1000) {
    // pulllifsSpikesFromDevice();
    // int spkcnt = getlifsCurrentSpikeCount();
    // cout<<"Spikes: "<<spkcnt<<endl;
    stepTime();
    ++steps;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto diff = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin);
  auto seconds = ((double) diff.count()) / 1e9;
  cout<<"Time taken: "<<setprecision(10)<<seconds<<"s, per step: "<<seconds * 1000 / steps<<"ms"<<endl;
}
