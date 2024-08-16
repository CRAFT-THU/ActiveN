#include "definitionsInternal.h"

extern "C" {
// ------------------------------------------------------------------------
// global variables
// ------------------------------------------------------------------------
unsigned long long iT;
float t;

// ------------------------------------------------------------------------
// timers
// ------------------------------------------------------------------------
double initTime = 0.0;
double initSparseTime = 0.0;
double neuronUpdateTime = 0.0;
double presynapticUpdateTime = 0.0;
double postsynapticUpdateTime = 0.0;
double synapseDynamicsTime = 0.0;
// ------------------------------------------------------------------------
// merged group arrays
// ------------------------------------------------------------------------
// ------------------------------------------------------------------------
// local neuron groups
// ------------------------------------------------------------------------
unsigned int* glbSpkCntlifs;
unsigned int* d_glbSpkCntlifs;
unsigned int* glbSpklifs;
unsigned int* d_glbSpklifs;
scalar* Vlifs;
scalar* d_Vlifs;
scalar* RefracTimelifs;
scalar* d_RefracTimelifs;

// ------------------------------------------------------------------------
// custom update variables
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// pre and postsynaptic variables
// ------------------------------------------------------------------------
float* inSynsyn;
float* d_inSynsyn;

// ------------------------------------------------------------------------
// synapse connectivity
// ------------------------------------------------------------------------
const unsigned int maxRowLengthsyn = 476;
unsigned int* rowLengthsyn;
unsigned int* d_rowLengthsyn;
uint32_t* indsyn;
uint32_t* d_indsyn;

// ------------------------------------------------------------------------
// synapse variables
// ------------------------------------------------------------------------
scalar* gsyn;
scalar* d_gsyn;

}  // extern "C"
// ------------------------------------------------------------------------
// extra global params
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// copying things to device
// ------------------------------------------------------------------------
void pushlifsSpikesToDevice(bool uninitialisedOnly) {
    if(!uninitialisedOnly) {
        CHECK_CUDA_ERRORS(cudaMemcpy(d_glbSpkCntlifs, glbSpkCntlifs, 1 * sizeof(unsigned int), cudaMemcpyHostToDevice));
    }
    if(!uninitialisedOnly) {
        CHECK_CUDA_ERRORS(cudaMemcpy(d_glbSpklifs, glbSpklifs, 82469 * sizeof(unsigned int), cudaMemcpyHostToDevice));
    }
}

void pushlifsCurrentSpikesToDevice(bool uninitialisedOnly) {
    CHECK_CUDA_ERRORS(cudaMemcpy(d_glbSpkCntlifs, glbSpkCntlifs, 1 * sizeof(unsigned int), cudaMemcpyHostToDevice));
    CHECK_CUDA_ERRORS(cudaMemcpy(d_glbSpklifs, glbSpklifs, glbSpkCntlifs[0] * sizeof(unsigned int), cudaMemcpyHostToDevice));
}

void pushVlifsToDevice(bool uninitialisedOnly) {
    CHECK_CUDA_ERRORS(cudaMemcpy(d_Vlifs, Vlifs, 82469 * sizeof(scalar), cudaMemcpyHostToDevice));
}

void pushCurrentVlifsToDevice(bool uninitialisedOnly) {
    CHECK_CUDA_ERRORS(cudaMemcpy(d_Vlifs, Vlifs, 82469 * sizeof(scalar), cudaMemcpyHostToDevice));
}

void pushRefracTimelifsToDevice(bool uninitialisedOnly) {
    if(!uninitialisedOnly) {
        CHECK_CUDA_ERRORS(cudaMemcpy(d_RefracTimelifs, RefracTimelifs, 82469 * sizeof(scalar), cudaMemcpyHostToDevice));
    }
}

void pushCurrentRefracTimelifsToDevice(bool uninitialisedOnly) {
    CHECK_CUDA_ERRORS(cudaMemcpy(d_RefracTimelifs, RefracTimelifs, 82469 * sizeof(scalar), cudaMemcpyHostToDevice));
}

void pushlifsStateToDevice(bool uninitialisedOnly) {
    pushVlifsToDevice(uninitialisedOnly);
    pushRefracTimelifsToDevice(uninitialisedOnly);
}

void pushsynConnectivityToDevice(bool uninitialisedOnly) {
    CHECK_CUDA_ERRORS(cudaMemcpy(d_rowLengthsyn, rowLengthsyn, 82469 * sizeof(unsigned int), cudaMemcpyHostToDevice));
    CHECK_CUDA_ERRORS(cudaMemcpy(d_indsyn, indsyn, 39255244 * sizeof(uint32_t), cudaMemcpyHostToDevice));
}

void pushgsynToDevice(bool uninitialisedOnly) {
    CHECK_CUDA_ERRORS(cudaMemcpy(d_gsyn, gsyn, 39255244 * sizeof(scalar), cudaMemcpyHostToDevice));
}

void pushinSynsynToDevice(bool uninitialisedOnly) {
    if(!uninitialisedOnly) {
        CHECK_CUDA_ERRORS(cudaMemcpy(d_inSynsyn, inSynsyn, 82469 * sizeof(float), cudaMemcpyHostToDevice));
    }
}

void pushsynStateToDevice(bool uninitialisedOnly) {
    pushgsynToDevice(uninitialisedOnly);
    pushinSynsynToDevice(uninitialisedOnly);
}


// ------------------------------------------------------------------------
// copying things from device
// ------------------------------------------------------------------------
void pulllifsSpikesFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(glbSpkCntlifs, d_glbSpkCntlifs, 1 * sizeof(unsigned int), cudaMemcpyDeviceToHost));
    CHECK_CUDA_ERRORS(cudaMemcpy(glbSpklifs, d_glbSpklifs, 82469 * sizeof(unsigned int), cudaMemcpyDeviceToHost));
}

void pulllifsCurrentSpikesFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(glbSpkCntlifs, d_glbSpkCntlifs, 1 * sizeof(unsigned int), cudaMemcpyDeviceToHost));
    CHECK_CUDA_ERRORS(cudaMemcpy(glbSpklifs, d_glbSpklifs, glbSpkCntlifs[0] * sizeof(unsigned int), cudaMemcpyDeviceToHost));
}

void pullVlifsFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(Vlifs, d_Vlifs, 82469 * sizeof(scalar), cudaMemcpyDeviceToHost));
}

void pullCurrentVlifsFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(Vlifs, d_Vlifs, 82469 * sizeof(scalar), cudaMemcpyDeviceToHost));
}

void pullRefracTimelifsFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(RefracTimelifs, d_RefracTimelifs, 82469 * sizeof(scalar), cudaMemcpyDeviceToHost));
}

void pullCurrentRefracTimelifsFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(RefracTimelifs, d_RefracTimelifs, 82469 * sizeof(scalar), cudaMemcpyDeviceToHost));
}

void pulllifsStateFromDevice() {
    pullVlifsFromDevice();
    pullRefracTimelifsFromDevice();
}

void pullsynConnectivityFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(rowLengthsyn, d_rowLengthsyn, 82469 * sizeof(unsigned int), cudaMemcpyDeviceToHost));
    CHECK_CUDA_ERRORS(cudaMemcpy(indsyn, d_indsyn, 39255244 * sizeof(uint32_t), cudaMemcpyDeviceToHost));
}

void pullgsynFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(gsyn, d_gsyn, 39255244 * sizeof(scalar), cudaMemcpyDeviceToHost));
}

void pullinSynsynFromDevice() {
    CHECK_CUDA_ERRORS(cudaMemcpy(inSynsyn, d_inSynsyn, 82469 * sizeof(float), cudaMemcpyDeviceToHost));
}

void pullsynStateFromDevice() {
    pullgsynFromDevice();
    pullinSynsynFromDevice();
}


// ------------------------------------------------------------------------
// helper getter functions
// ------------------------------------------------------------------------
unsigned int* getlifsCurrentSpikes(unsigned int batch) {
    return (glbSpklifs);
}

unsigned int& getlifsCurrentSpikeCount(unsigned int batch) {
    return glbSpkCntlifs[0];
}

scalar* getCurrentVlifs(unsigned int batch) {
    return Vlifs;
}

scalar* getCurrentRefracTimelifs(unsigned int batch) {
    return RefracTimelifs;
}


void copyStateToDevice(bool uninitialisedOnly) {
    pushlifsStateToDevice(uninitialisedOnly);
    pushsynStateToDevice(uninitialisedOnly);
}

void copyConnectivityToDevice(bool uninitialisedOnly) {
    pushsynConnectivityToDevice(uninitialisedOnly);
}

void copyStateFromDevice() {
    pulllifsStateFromDevice();
    pullsynStateFromDevice();
}

void copyCurrentSpikesFromDevice() {
    pulllifsCurrentSpikesFromDevice();
}

void copyCurrentSpikeEventsFromDevice() {
}

void allocateMem() {
    int deviceID;
    CHECK_CUDA_ERRORS(cudaDeviceGetByPCIBusId(&deviceID, "0000:01:00.0"));
    CHECK_CUDA_ERRORS(cudaSetDevice(deviceID));
    
    // ------------------------------------------------------------------------
    // global variables
    // ------------------------------------------------------------------------
    
    // ------------------------------------------------------------------------
    // timers
    // ------------------------------------------------------------------------
    // ------------------------------------------------------------------------
    // local neuron groups
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaHostAlloc(&glbSpkCntlifs, 1 * sizeof(unsigned int), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_glbSpkCntlifs, 1 * sizeof(unsigned int)));
    CHECK_CUDA_ERRORS(cudaHostAlloc(&glbSpklifs, 82469 * sizeof(unsigned int), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_glbSpklifs, 82469 * sizeof(unsigned int)));
    CHECK_CUDA_ERRORS(cudaHostAlloc(&Vlifs, 82469 * sizeof(scalar), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_Vlifs, 82469 * sizeof(scalar)));
    CHECK_CUDA_ERRORS(cudaHostAlloc(&RefracTimelifs, 82469 * sizeof(scalar), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_RefracTimelifs, 82469 * sizeof(scalar)));
    
    // ------------------------------------------------------------------------
    // custom update variables
    // ------------------------------------------------------------------------
    
    // ------------------------------------------------------------------------
    // pre and postsynaptic variables
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaHostAlloc(&inSynsyn, 82469 * sizeof(float), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_inSynsyn, 82469 * sizeof(float)));
    
    // ------------------------------------------------------------------------
    // synapse connectivity
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaHostAlloc(&rowLengthsyn, 82469 * sizeof(unsigned int), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_rowLengthsyn, 82469 * sizeof(unsigned int)));
    CHECK_CUDA_ERRORS(cudaHostAlloc(&indsyn, 39255244 * sizeof(uint32_t), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_indsyn, 39255244 * sizeof(uint32_t)));
    
    // ------------------------------------------------------------------------
    // synapse variables
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaHostAlloc(&gsyn, 39255244 * sizeof(scalar), cudaHostAllocPortable));
    CHECK_CUDA_ERRORS(cudaMalloc(&d_gsyn, 39255244 * sizeof(scalar)));
    
    pushMergedNeuronInitGroup0ToDevice(0, d_glbSpkCntlifs, d_glbSpklifs, d_RefracTimelifs, d_inSynsyn, 82469);
    pushMergedNeuronUpdateGroup0ToDevice(0, d_glbSpkCntlifs, d_glbSpklifs, d_Vlifs, d_RefracTimelifs, d_inSynsyn, 82469);
    pushMergedPresynapticUpdateGroup0ToDevice(0, d_inSynsyn, d_glbSpkCntlifs, d_glbSpklifs, d_rowLengthsyn, d_indsyn, d_gsyn, 82469, 82469, 476);
    pushMergedNeuronSpikeQueueUpdateGroup0ToDevice(0, d_glbSpkCntlifs);
}

void freeMem() {
    // ------------------------------------------------------------------------
    // global variables
    // ------------------------------------------------------------------------
    
    // ------------------------------------------------------------------------
    // timers
    // ------------------------------------------------------------------------
    // ------------------------------------------------------------------------
    // local neuron groups
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaFreeHost(glbSpkCntlifs));
    CHECK_CUDA_ERRORS(cudaFree(d_glbSpkCntlifs));
    CHECK_CUDA_ERRORS(cudaFreeHost(glbSpklifs));
    CHECK_CUDA_ERRORS(cudaFree(d_glbSpklifs));
    CHECK_CUDA_ERRORS(cudaFreeHost(Vlifs));
    CHECK_CUDA_ERRORS(cudaFree(d_Vlifs));
    CHECK_CUDA_ERRORS(cudaFreeHost(RefracTimelifs));
    CHECK_CUDA_ERRORS(cudaFree(d_RefracTimelifs));
    
    // ------------------------------------------------------------------------
    // custom update variables
    // ------------------------------------------------------------------------
    
    // ------------------------------------------------------------------------
    // pre and postsynaptic variables
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaFreeHost(inSynsyn));
    CHECK_CUDA_ERRORS(cudaFree(d_inSynsyn));
    
    // ------------------------------------------------------------------------
    // synapse connectivity
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaFreeHost(rowLengthsyn));
    CHECK_CUDA_ERRORS(cudaFree(d_rowLengthsyn));
    CHECK_CUDA_ERRORS(cudaFreeHost(indsyn));
    CHECK_CUDA_ERRORS(cudaFree(d_indsyn));
    
    // ------------------------------------------------------------------------
    // synapse variables
    // ------------------------------------------------------------------------
    CHECK_CUDA_ERRORS(cudaFreeHost(gsyn));
    CHECK_CUDA_ERRORS(cudaFree(d_gsyn));
    
}

size_t getFreeDeviceMemBytes() {
    size_t free;
    size_t total;
    CHECK_CUDA_ERRORS(cudaMemGetInfo(&free, &total));
    return free;
}

void stepTime() {
    updateSynapses(t);
    updateNeurons(t); 
    iT++;
    t = iT*DT;
}

