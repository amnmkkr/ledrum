/* Shim config.h for LXR data files on non-STM32 targets */
#ifndef CONFIG_SOUNDENGINE_H_
#define CONFIG_SOUNDENGINE_H_

#include <stdint.h>

#define OUTPUT_DMA_SIZE   64
#define REAL_FS           44140.0f
#define REAL_FS2          (REAL_FS / 2.0f)

#define USE_BIG_SAMPLE_BUFFER     1
#define INTERPOLATE_OSC           1
#define INTERPOLATE_FM_OSC        1
#define ENABLE_MIX_OSC            1
#define ENABLE_DRUM_SVF           1
#define USE_FILTER_DRIVE          0
#define AMP_EG_SYNC               0
#define SMOOTH_MIXER_VOL_PAN_DRIVE 1

/* Strip DTCM section attributes */
#define INDTCM
#define INDTCMZ

#endif /* CONFIG_SOUNDENGINE_H_ */
