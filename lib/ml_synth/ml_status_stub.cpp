/**
 * @file ml_status_stub.cpp
 * @brief PC stub implementations of ML_SynthTools Status_* functions
 *
 * The original ML_SynthTools provides these via ml_status.cpp which depends
 * on Arduino Serial. This stub uses printf instead.
 */

#include "ml_status.h"
#include <cstdio>

void Status_Setup(void) {}

void Status_Init(uint32_t sample_rate)
{
    printf("[ML Status] Init (sr=%u)\n", sample_rate);
}

void Status_Process(void) {}

void Status_Process_Sample(uint32_t /*count*/) {}

void Status_ValueChangedFloat(const char *group, const char *descr, float value)
{
    printf("[ML Status] %s/%s = %.4f\n", group, descr, value);
}

void Status_ValueChangedFloat(const char *descr, float value)
{
    printf("[ML Status] %s = %.4f\n", descr, value);
}

void Status_ValueChangedFloatArr(const char *descr, float value, int index)
{
    printf("[ML Status] %s[%d] = %.4f\n", descr, index, value);
}

void Status_ValueChangedIntArr(const char *descr, int value, int index)
{
    printf("[ML Status] %s[%d] = %d\n", descr, index, value);
}

void Status_ValueChangedIntArr(const char *group, const char *descr, int value, int index)
{
    printf("[ML Status] %s/%s[%d] = %d\n", group, descr, index, value);
}

void Status_ValueChangedInt(const char *group, const char *descr, int value)
{
    printf("[ML Status] %s/%s = %d\n", group, descr, value);
}

void Status_ValueChangedInt(const char *descr, int value)
{
    printf("[ML Status] %s = %d\n", descr, value);
}

void Status_ValueChangedStr(const char *descr, const char *value)
{
    printf("[ML Status] %s = %s\n", descr, value);
}

void Status_ValueChangedStr(const char *group, const char *descr, const char *value)
{
    printf("[ML Status] %s/%s = %s\n", group, descr, value);
}

void Status_LogMessage(const char *text)
{
    printf("[ML Status] %s", text);
}
