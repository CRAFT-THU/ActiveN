#pragma once
#define EXPORT_VAR extern
#define EXPORT_FUNC
// Standard C++ includes
#include <random>
#include <string>
#include <stdexcept>

// Standard C includes
#include <cassert>
#include <cstdint>
#define DT 1.00000000000000006e-01f
typedef float scalar;
#define SCALAR_MIN 1.175494351e-38f
#define SCALAR_MAX 3.402823466e+38f

#define TIME_MIN 1.175494351e-38f
#define TIME_MAX 3.402823466e+38f

// ------------------------------------------------------------------------
// bit tool macros
#define B(x,i) ((x) & (0x80000000 >> (i))) //!< Extract the bit at the specified position i from x
#define setB(x,i) x= ((x) | (0x80000000 >> (i))) //!< Set the bit at the specified position i in x to 1
#define delB(x,i) x= ((x) & (~(0x80000000 >> (i)))) //!< Set the bit at the specified position i in x to 0

extern "C" {
// ------------------------------------------------------------------------
// global variables
// ------------------------------------------------------------------------
EXPORT_VAR unsigned long long iT;
EXPORT_VAR float t;

// ------------------------------------------------------------------------
// timers
// ------------------------------------------------------------------------
EXPORT_VAR double initTime;
EXPORT_VAR double initSparseTime;
EXPORT_VAR double neuronUpdateTime;
EXPORT_VAR double presynapticUpdateTime;
EXPORT_VAR double postsynapticUpdateTime;
EXPORT_VAR double synapseDynamicsTime;
// ------------------------------------------------------------------------
// local neuron groups
// ------------------------------------------------------------------------
#define spikeCount_lifs glbSpkCntlifs[0]
#define spike_lifs glbSpklifs
#define glbSpkShiftlifs 0

EXPORT_VAR unsigned int* glbSpkCntlifs;
EXPORT_VAR unsigned int* d_glbSpkCntlifs;
EXPORT_VAR unsigned int* glbSpklifs;
EXPORT_VAR unsigned int* d_glbSpklifs;
EXPORT_VAR scalar* Vlifs;
EXPORT_VAR scalar* d_Vlifs;
EXPORT_VAR scalar* RefracTimelifs;
EXPORT_VAR scalar* d_RefracTimelifs;

// ------------------------------------------------------------------------
// custom update variables
// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
// pre and postsynaptic variables
// ------------------------------------------------------------------------
EXPORT_VAR float* inSynsyn;
EXPORT_VAR float* d_inSynsyn;

// ------------------------------------------------------------------------
// synapse connectivity
// ------------------------------------------------------------------------
EXPORT_VAR const unsigned int maxRowLengthsyn;
EXPORT_VAR unsigned int* rowLengthsyn;
EXPORT_VAR unsigned int* d_rowLengthsyn;
EXPORT_VAR uint32_t* indsyn;
EXPORT_VAR uint32_t* d_indsyn;

// ------------------------------------------------------------------------
// synapse variables
// ------------------------------------------------------------------------
EXPORT_VAR scalar* gsyn;
EXPORT_VAR scalar* d_gsyn;

EXPORT_FUNC void pushlifsSpikesToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pulllifsSpikesFromDevice();
EXPORT_FUNC void pushlifsCurrentSpikesToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pulllifsCurrentSpikesFromDevice();
EXPORT_FUNC unsigned int* getlifsCurrentSpikes(unsigned int batch = 0); 
EXPORT_FUNC unsigned int& getlifsCurrentSpikeCount(unsigned int batch = 0); 
EXPORT_FUNC void pushVlifsToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullVlifsFromDevice();
EXPORT_FUNC void pushCurrentVlifsToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullCurrentVlifsFromDevice();
EXPORT_FUNC scalar* getCurrentVlifs(unsigned int batch = 0); 
EXPORT_FUNC void pushRefracTimelifsToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullRefracTimelifsFromDevice();
EXPORT_FUNC void pushCurrentRefracTimelifsToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullCurrentRefracTimelifsFromDevice();
EXPORT_FUNC scalar* getCurrentRefracTimelifs(unsigned int batch = 0); 
EXPORT_FUNC void pushlifsStateToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pulllifsStateFromDevice();
EXPORT_FUNC void pushsynConnectivityToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullsynConnectivityFromDevice();
EXPORT_FUNC void pushgsynToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullgsynFromDevice();
EXPORT_FUNC void pushinSynsynToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullinSynsynFromDevice();
EXPORT_FUNC void pushsynStateToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void pullsynStateFromDevice();
// Runner functions
EXPORT_FUNC void copyStateToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void copyConnectivityToDevice(bool uninitialisedOnly = false);
EXPORT_FUNC void copyStateFromDevice();
EXPORT_FUNC void copyCurrentSpikesFromDevice();
EXPORT_FUNC void copyCurrentSpikeEventsFromDevice();
EXPORT_FUNC void allocateMem();
EXPORT_FUNC void freeMem();
EXPORT_FUNC size_t getFreeDeviceMemBytes();
EXPORT_FUNC void stepTime();

// Functions generated by backend
EXPORT_FUNC void updateNeurons(float t); 
EXPORT_FUNC void updateSynapses(float t);
EXPORT_FUNC void initialize();
EXPORT_FUNC void initializeSparse();
}  // extern "C"
