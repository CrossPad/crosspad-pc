/**
 * @file ml_compat.h
 * @brief MSVC compatibility shim for ML_SynthTools vendored sources
 *
 * Handles GCC-specific attributes and missing defines on MSVC.
 */

#ifndef ML_COMPAT_H_
#define ML_COMPAT_H_

/* MSVC does not support __attribute__ — make it a no-op */
#ifdef _MSC_VER
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif

/* MSVC requires _USE_MATH_DEFINES for M_PI from <cmath>/<math.h> */
#ifdef _MSC_VER
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#endif

#endif /* ML_COMPAT_H_ */
