#include "definitionsInternal.h"
#include "supportCode.h"

struct MergedNeuronUpdateGroup0
 {
    unsigned int* spkCnt;
    unsigned int* spk;
    scalar* V;
    scalar* RefracTime;
    float* inSynInSyn0;
    unsigned int numNeurons;
    
}
;
struct MergedNeuronSpikeQueueUpdateGroup0
 {
    unsigned int* spkCnt;
    
}
;
__device__ __constant__ MergedNeuronSpikeQueueUpdateGroup0 d_mergedNeuronSpikeQueueUpdateGroup0[1];
void pushMergedNeuronSpikeQueueUpdateGroup0ToDevice(unsigned int idx, unsigned int* spkCnt) {
    MergedNeuronSpikeQueueUpdateGroup0 group = {spkCnt, };
    CHECK_CUDA_ERRORS(cudaMemcpyToSymbolAsync(d_mergedNeuronSpikeQueueUpdateGroup0, &group, sizeof(MergedNeuronSpikeQueueUpdateGroup0), idx * sizeof(MergedNeuronSpikeQueueUpdateGroup0)));
}
__device__ __constant__ MergedNeuronUpdateGroup0 d_mergedNeuronUpdateGroup0[1];
void pushMergedNeuronUpdateGroup0ToDevice(unsigned int idx, unsigned int* spkCnt, unsigned int* spk, scalar* V, scalar* RefracTime, float* inSynInSyn0, unsigned int numNeurons) {
    MergedNeuronUpdateGroup0 group = {spkCnt, spk, V, RefracTime, inSynInSyn0, numNeurons, };
    CHECK_CUDA_ERRORS(cudaMemcpyToSymbolAsync(d_mergedNeuronUpdateGroup0, &group, sizeof(MergedNeuronUpdateGroup0), idx * sizeof(MergedNeuronUpdateGroup0)));
}
// ------------------------------------------------------------------------
// merged extra global parameter functions
// ------------------------------------------------------------------------
// ------------------------------------------------------------------------
// merged extra global parameter functions
// ------------------------------------------------------------------------
__device__ __constant__ unsigned int d_mergedNeuronUpdateGroupStartID0[] = {0, };

extern "C" __global__ void neuronSpikeQueueUpdateKernel() {
    const unsigned int id = 32 * blockIdx.x + threadIdx.x;
    if(id < 1) {
        struct MergedNeuronSpikeQueueUpdateGroup0 *group = &d_mergedNeuronSpikeQueueUpdateGroup0[id - 0]; 
        group->spkCnt[0] = 0;
    }
}

extern "C" __global__ void updateNeuronsKernel(float t)
 {
    const unsigned int id = 32 * blockIdx.x + threadIdx.x; 
    __shared__ unsigned int shSpk[32];
    __shared__ unsigned int shPosSpk;
    __shared__ unsigned int shSpkCount;
    if (threadIdx.x == 0) {
        shSpkCount = 0;
    }
    
    __syncthreads();
    // merged0
    if(id < 82496) {
        struct MergedNeuronUpdateGroup0 *group = &d_mergedNeuronUpdateGroup0[0]; 
        const unsigned int lid = id - 0;
        
        if(lid < group->numNeurons) {
            scalar lV = group->V[lid];
            scalar lRefracTime = group->RefracTime[lid];
            
            float Isyn = 0;
             {
                // pull inSyn values in a coalesced access
                float linSyn = group->inSynInSyn0[lid];
                Isyn += linSyn; linSyn = 0;
                
                group->inSynInSyn0[lid] = linSyn;
            }
            // test whether spike condition was fulfilled previously
            // calculate membrane potential
            if (lRefracTime <= 0.0f) {
              scalar alpha = ((Isyn + (0.00000000000000000e+00f)) * (1.00000000000000000e+00f)) + (0.00000000000000000e+00f);
              lV = alpha - ((9.04837418035959518e-01f) * (alpha - lV));
            }
            else {
              lRefracTime -= DT;
            }
            
            // test for and register a true spike
            if (lRefracTime <= 0.0f && lV >= (1.50000000000000000e+01f)) {
                const unsigned int spkIdx = atomicAdd(&shSpkCount, 1);
                shSpk[spkIdx] = lid;
                // spike reset code
                lV = (0.00000000000000000e+00f);
                lRefracTime = (0.00000000000000000e+00f);
                
            }
            group->V[lid] = lV;
            group->RefracTime[lid] = lRefracTime;
        }
        __syncthreads();
        if(threadIdx.x == 0) {
            if (shSpkCount > 0) {
                shPosSpk = atomicAdd(&group->spkCnt[0], shSpkCount);
            }
        }
        __syncthreads();
        if(threadIdx.x < shSpkCount) {
            const unsigned int n = shSpk[threadIdx.x];
            group->spk[shPosSpk + threadIdx.x] = n;
        }
    }
}
void updateNeurons(float t) {
     {
        const dim3 threads(32, 1);
        const dim3 grid(1, 1);
        neuronSpikeQueueUpdateKernel<<<grid, threads>>>();
        CHECK_CUDA_ERRORS(cudaPeekAtLastError());
    }
     {
        const dim3 threads(32, 1);
        const dim3 grid(2578, 1);
        updateNeuronsKernel<<<grid, threads>>>(t);
        CHECK_CUDA_ERRORS(cudaPeekAtLastError());
    }
}
