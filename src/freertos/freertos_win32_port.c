/**
 * @file    Event management with Win32 API
 * @brief   Implementation of an event mechanism using Windows synchronization primitives.
 *          Windows equivalent of freertos_posix_port.c (which uses pthreads).
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* Structure representing an event, using Win32 CRITICAL_SECTION and CONDITION_VARIABLE */
typedef struct Event
{
    CONDITION_VARIABLE cond;
    CRITICAL_SECTION cs;
    bool signaled;
} Event_t;

Event_t *event_create(void)
{
    Event_t *event = (Event_t *)malloc(sizeof(Event_t));
    if (event)
    {
        InitializeConditionVariable(&event->cond);
        InitializeCriticalSection(&event->cs);
        event->signaled = false;
    }
    return event;
}

void event_delete(Event_t *event)
{
    if (event)
    {
        DeleteCriticalSection(&event->cs);
        free(event);
    }
}

void event_signal(Event_t *event)
{
    if (event)
    {
        EnterCriticalSection(&event->cs);
        event->signaled = true;
        WakeConditionVariable(&event->cond);
        LeaveCriticalSection(&event->cs);
    }
}

void event_wait(Event_t *event)
{
    if (event)
    {
        EnterCriticalSection(&event->cs);
        while (!event->signaled)
        {
            SleepConditionVariableCS(&event->cond, &event->cs, INFINITE);
        }
        event->signaled = false;
        LeaveCriticalSection(&event->cs);
    }
}
