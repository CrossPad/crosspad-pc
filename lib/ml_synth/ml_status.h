/*
 * Copyright (c) 2025 Marcel Licence
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file    ml_status.h
 * @author  Marcel Licence
 * @date    06.01.2022
 *
 * @brief   This file contains the prototypes of functions for status messages generation
 *
 * Vendored from ML_SynthTools — added ml_compat.h include for MSVC __attribute__ fix.
 */


#ifndef SRC_ML_STATUS_H_
#define SRC_ML_STATUS_H_

#include "ml_compat.h"

#include <stdint.h>


void Status_Setup(void);
void Status_Init(uint32_t sample_rate);
void Status_Process(void);
void Status_Process_Sample(uint32_t count __attribute__((unused)));
void Status_ValueChangedFloat(const char *group, const char *descr, float value);
void Status_ValueChangedFloat(const char *descr, float value);
void Status_ValueChangedFloatArr(const char *descr, float value, int index);
void Status_ValueChangedIntArr(const char *descr, int value, int index);
void Status_ValueChangedIntArr(const char *group, const char *descr, int value, int index);
void Status_ValueChangedInt(const char *group, const char *descr, int value);
void Status_ValueChangedInt(const char *descr, int value);
void Status_ValueChangedStr(const char *descr, const char *value);
void Status_ValueChangedStr(const char *group, const char *descr, const char *value);
void Status_LogMessage(const char *text);


#ifdef STATUS_SIMPLE
void Status_Loop(uint32_t elapsed_time);
void Status_LoopMain();
#endif


#endif /* SRC_ML_STATUS_H_ */
