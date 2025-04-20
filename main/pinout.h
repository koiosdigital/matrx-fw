#ifndef PINOUT_H
#define PINOUT_H

#if CONFIG_HW_TYPE == 1
#include "hw_defs/matrx_v9.h"
#endif

#if CONFIG_HW_TYPE == 2
#include "hw_defs/matrx_v8.h"
#endif

#if CONFIG_HW_TYPE == 3
#include "hw_defs/tidbyt_v1.h"
#endif

#if CONFIG_HW_TYPE == 4
#include "hw_defs/tidbyt_v2.h"
#endif

#endif