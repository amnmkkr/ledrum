/* Shim transientTables.h — matches LXR's include path */
#ifndef TRANSIENTTABLES_H_
#define TRANSIENTTABLES_H_

#include <stdint.h>

#define NUM_TRANSIENTS 12
#define TRANSIENT_SAMPLE_LENGTH 2205

extern const float transientVolumeTable[69];
extern const int8_t transientData[NUM_TRANSIENTS][TRANSIENT_SAMPLE_LENGTH];

#endif /* TRANSIENTTABLES_H_ */
