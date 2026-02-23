/**
 * @file ml_utils_stub.cpp
 * @brief PC stub implementations of ML_SynthTools utility functions
 *
 * Provides the non-inline functions declared in ml_utils.h.
 * The original lives in ML_SynthTools/src/ml_utils.cpp.
 */

#include "ml_utils.h"
#include <cmath>
#include <cstdint>

float maxAbsValueFromSampleBuffer(float *samples, uint16_t sample_count)
{
    float maxVal = 0.0f;
    for (uint16_t i = 0; i < sample_count; i++)
    {
        float v = std::fabs(samples[i]);
        if (v > maxVal) maxVal = v;
    }
    return maxVal;
}

float maxValueFromSampleBuffer(float *samples, uint16_t sample_count)
{
    float maxVal = -1e30f;
    for (uint16_t i = 0; i < sample_count; i++)
    {
        if (samples[i] > maxVal) maxVal = samples[i];
    }
    return maxVal;
}

float minValueFromSampleBuffer(float *samples, uint16_t sample_count)
{
    float minVal = 1e30f;
    for (uint16_t i = 0; i < sample_count; i++)
    {
        if (samples[i] < minVal) minVal = samples[i];
    }
    return minVal;
}

float log16bit(float f)
{
    return std::log2(f + 1.0f) / 16.0f;
}

float floatFromU7(uint8_t value)
{
    return (float)value / 127.0f;
}

float log2fromU7(uint8_t value, float minExp, float maxExp)
{
    float normalized = (float)value / 127.0f;
    float exponent = minExp + normalized * (maxExp - minExp);
    return std::pow(2.0f, exponent);
}

float log10fromU7(uint8_t value, float minExp, float maxExp)
{
    float normalized = (float)value / 127.0f;
    float exponent = minExp + normalized * (maxExp - minExp);
    return std::pow(10.0f, exponent);
}

float log10fromU7val(uint8_t value, float minVal, float maxVal)
{
    float normalized = (float)value / 127.0f;
    return minVal + normalized * (maxVal - minVal);
}
