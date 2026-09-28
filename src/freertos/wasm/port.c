/**
 * FreeRTOS port for the browser (Emscripten, no pthreads).
 *
 * Every task is an Emscripten fiber and the scheduler is cooperative: a task
 * runs until it blocks or yields, and vPortYield() swaps straight to the fiber
 * FreeRTOS picked next. The browser's own thread is one more fiber, "main":
 * the idle task swaps back to it, main sleeps (emscripten_sleep → the page
 * gets its event loop back), counts the milliseconds that passed as ticks and
 * swaps into whichever task those ticks woke. Needs -sASYNCIFY.
 */
#include <emscripten.h>
#include <emscripten/fiber.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#define FIBER_C_STACK      ( 1024 * 1024 )
#define FIBER_ASYNC_STACK  ( 512 * 1024 )
#define MAIN_SLEEP_MS      4
#define MAX_TICKS_PER_WAKE 1000

typedef struct
{
    emscripten_fiber_t fiber;
    void * cstack;
    void * astack;
    TaskFunction_t code;
    void * params;
} Thread_t;

static inline Thread_t * prvGetThreadFromTask( TaskHandle_t xTask )
{
    StackType_t * pxTopOfStack = *( StackType_t ** ) xTask;

    return ( Thread_t * ) ( pxTopOfStack + 1 );
}

static emscripten_fiber_t xMainFiber;
static char ucMainAsyncStack[ FIBER_ASYNC_STACK ];
static emscripten_fiber_t * pxRunning = NULL;
static volatile UBaseType_t uxCriticalNesting = 0;
static BaseType_t xSchedulerRunning = pdFALSE;
static double dLastTickMs = 0;

static void prvSwitchTo( emscripten_fiber_t * pxNext )
{
    emscripten_fiber_t * pxPrev = pxRunning;

    if( pxNext == pxPrev )
    {
        return;
    }

    pxRunning = pxNext;
    emscripten_fiber_swap( pxPrev, pxNext );
}

static void prvTaskEntry( void * pvArg )
{
    Thread_t * thread = ( Thread_t * ) pvArg;

    thread->code( thread->params );
    /* A task function that returns deletes itself, as on the board. */
    vTaskDelete( NULL );

    for( ; ; )
    {
        vPortYield();
    }
}

StackType_t * pxPortInitialiseStack( StackType_t * pxTopOfStack,
                                     TaskFunction_t pxCode,
                                     void * pvParameters )
{
    Thread_t * thread = ( Thread_t * ) ( pxTopOfStack + 1 ) - 1;

    memset( thread, 0, sizeof( *thread ) );
    thread->code = pxCode;
    thread->params = pvParameters;
    thread->cstack = malloc( FIBER_C_STACK );
    thread->astack = malloc( FIBER_ASYNC_STACK );
    configASSERT( thread->cstack && thread->astack );
    emscripten_fiber_init( &thread->fiber, prvTaskEntry, thread,
                           thread->cstack, FIBER_C_STACK,
                           thread->astack, FIBER_ASYNC_STACK );

    return ( StackType_t * ) thread - 1;
}

BaseType_t xPortStartScheduler( void )
{
    emscripten_fiber_init_from_current_context( &xMainFiber, ucMainAsyncStack, sizeof( ucMainAsyncStack ) );
    pxRunning = &xMainFiber;
    xSchedulerRunning = pdTRUE;
    dLastTickMs = emscripten_get_now();

    for( ; ; )
    {
        /* Run tasks until every one of them is blocked (the idle task hands
         * the CPU back here), then let the browser breathe. */
        prvSwitchTo( &prvGetThreadFromTask( xTaskGetCurrentTaskHandle() )->fiber );
        emscripten_sleep( MAIN_SLEEP_MS );

        double dNow = emscripten_get_now();
        int iTicks = ( int ) ( ( dNow - dLastTickMs ) * configTICK_RATE_HZ / 1000.0 );

        if( iTicks > MAX_TICKS_PER_WAKE )
        {
            dLastTickMs = dNow - ( double ) MAX_TICKS_PER_WAKE * 1000.0 / configTICK_RATE_HZ;
            iTicks = MAX_TICKS_PER_WAKE;
        }

        dLastTickMs += ( double ) iTicks * 1000.0 / configTICK_RATE_HZ;

        while( iTicks-- > 0 )
        {
            ( void ) xTaskIncrementTick();
        }

        vTaskSwitchContext();
    }

    return pdFALSE;
}

void vPortEndScheduler( void )
{
    xSchedulerRunning = pdFALSE;
}

void vPortYield( void )
{
    if( xSchedulerRunning == pdFALSE || pxRunning == &xMainFiber )
    {
        return;
    }

    vTaskSwitchContext();
    prvSwitchTo( &prvGetThreadFromTask( xTaskGetCurrentTaskHandle() )->fiber );
}

void vPortWasmIdle( void )
{
    if( xSchedulerRunning != pdFALSE && pxRunning != &xMainFiber )
    {
        prvSwitchTo( &xMainFiber );
    }
}

/* pdTRUE while a task fiber (not the page's own context) is running. */
BaseType_t xPortWasmInTask( void )
{
    return ( xSchedulerRunning != pdFALSE && pxRunning != &xMainFiber ) ? pdTRUE : pdFALSE;
}

void vPortCancelThread( void * pxTaskToDelete )
{
    Thread_t * thread = prvGetThreadFromTask( ( TaskHandle_t ) pxTaskToDelete );

    configASSERT( &thread->fiber != pxRunning );
    free( thread->cstack );
    free( thread->astack );
    thread->cstack = thread->astack = NULL;
}

void vPortEnterCritical( void )
{
    uxCriticalNesting++;
}

void vPortExitCritical( void )
{
    if( uxCriticalNesting > 0 )
    {
        uxCriticalNesting--;
    }
}

void vPortDisableInterrupts( void )
{
}

void vPortEnableInterrupts( void )
{
}

UBaseType_t xPortSetInterruptMask( void )
{
    return 0;
}

void vPortClearInterruptMask( UBaseType_t uxMask )
{
    ( void ) uxMask;
}

uint32_t ulPortGetRunTime( void )
{
    return ( uint32_t ) emscripten_get_now();
}
