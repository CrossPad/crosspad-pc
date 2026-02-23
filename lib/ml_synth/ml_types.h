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
 * @file ml_types.h
 * @author Marcel Licence
 * @date 17.01.2023
 *
 * @brief This file contains some datatypes
 *
 * Vendored from ML_SynthTools — unmodified.
 */


#ifndef SRC_ML_TYPES_H_
#define SRC_ML_TYPES_H_


#include <stdint.h>


/*
 * fixed point data types for signed numbers -1 .. 1
 */
typedef union
{
    struct
    {
        uint16_t n: 14, m: 1, s: 1;
    };
    int16_t s16;
} Q1_14;


#endif /* SRC_ML_TYPES_H_ */
