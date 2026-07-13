/*
 * Boot ROM embedding for the peripheral device.
 *
 * Split out from devices.h so the #embed of the build-generated bootrom.bin
 * (assembled from sim/bootrom.S; see sim/CMakeLists.txt) lives in a single
 * translation unit whose object depends on that binary.
 */
#include "devices.h"

// Default boot ROM image: sim/bootrom.S assembled and objcopy'd to raw binary
// in the build directory, which is on the include path (see CMakeLists.txt).
static const unsigned char kDefaultBootrom[] = {
#embed "bootrom.bin"
};

void PeripheralDevice::loadDefaultBootrom() {
  bootrom.assign(kDefaultBootrom, kDefaultBootrom + sizeof(kDefaultBootrom));
}
