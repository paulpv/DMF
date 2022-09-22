/*++

    Copyright (c) Microsoft Corporation. All rights reserved.
    Licensed under the MIT license.

Module Name:

    Dmf_ScheduledTask.c

Abstract:

    This Module allows Clients to schedule work that will completed a SINGLE time, either
    for the duration of the Driver's runtime or persistent over reboots.
    NOTE: A better name for this Module is "ScheduleTaskOnce".

Environment:

    Kernel-mode Driver Framework
    User-mode Driver Framework

--*/

// DMF and this Module's Library specific definitions.
//
#include "DmfModule.h"
#include "DmfModules.Library.h"
#include "DmfModules.Library.Trace.h"

#if defined(DMF_INCLUDE_TMH)
#include "Dmf_ScheduledTask.tmh"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////
// Module Private Enumerations and Structures
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

///////////////////////////////////////////////////////////////////////////////////////////////////////
// Module Private Context
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

typedef struct _DMF_CONTEXT_ScheduledTask
{
    // It is for the deferred callback.
    //
    WDFTIMER Timer;
    // Indicates if the work the Client wants to do has been done.
    //
    ULONG WorkIsCompleted;
    // Try again timer has started to allow for cases where success happens
    // but Client wants to try again.
    //
    BOOLEAN TimerIsStarted;

    // On Demand support.
    // -----------------
    // Workitem for running the ScheduledTask handler deferred on demand without affecting
    // the rest of the object.
    //
    WDFWORKITEM DeferredOnDemand;

    // Caller's context for On Demand calls.
    // NOTE: This is only really useful in the case where a single call is made.
    //       If multiple calls are made, then the context passed will be for the
    //       first call. (Essentially it is only used to determine if the call
    //       is On Demand or not.)
    //
    VOID* OnDemandCallbackContext;
    // Indicates the number of callers that are waiting for the 
    // timer. When this variable changes from 0 to 1, the On Demand
    // timer is started; otherwise, nothing happens since the timer
    // routine will execute.
    //
    LONG NumberOfPendingCalls;

    // For Synchronization with Canceling.
    //
    DMFMODULE DmfModuleRundown;

    // Prevent timer from restarting during ReleaseHardware/D0Exit so the callback
    // is minimized from happening during D0Entry/PrepareHardware.
    // (Maintain legacy behavior. In rare cases callback can happen at the same
    // time. It may be necessary for the Client to synchronize inside the callback.
    //
    BOOLEAN DisableRetries;
} DMF_CONTEXT_ScheduledTask;

// This macro declares the following function:
// DMF_CONTEXT_GET()
//
DMF_MODULE_DECLARE_CONTEXT(ScheduledTask)

// This macro declares the following function:
// DMF_CONFIG_GET()
//
DMF_MODULE_DECLARE_CONFIG(ScheduledTask)

// Memory Pool Tag.
//
#define MemoryTag 'oMTS'

///////////////////////////////////////////////////////////////////////////////////////////////////////
// DMF Module Support Code
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

// The name of the default variable.
//
#define DEFAULT_NAME_DEVICE    L"TimesRun"

VOID
ScheduledTask_TimerRestart(
    _In_ DMFMODULE DmfModule,
    _In_ ScheduledTask_Result_Type WorkResult
    )
/*++

Routine Description:

    Common routine to restart the timer.

Parameters:

    DmfModule - This Module's handle.
    WorkResult - The Client's previous return from its callback.

Return:

    None

--*/
{
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;
    NTSTATUS ntStatus;

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    // D0Exit/ReleaseHardware has happened. Don't restart the timer. It will
    // restart during resume.
    //
    if (moduleContext->DisableRetries)
    {
        goto Exit;
    }

    ntStatus = DMF_Rundown_Reference(moduleContext->DmfModuleRundown);
    if (NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Timer RESTART");

        moduleContext->TimerIsStarted = TRUE;

        ULONG timerPeriodMs;

        if (WorkResult == ScheduledTask_WorkResult_SuccessButTryAgain)
        {
            timerPeriodMs = moduleConfig->TimerPeriodMsOnSuccess;
        }
        else if (WorkResult == ScheduledTask_WorkResult_FailButTryAgain)
        {
            timerPeriodMs = moduleConfig->TimerPeriodMsOnFail;
        }
        else
        {
            DmfAssert(FALSE);
            timerPeriodMs = 0;
        }

        WdfTimerStart(moduleContext->Timer,
                      WDF_REL_TIMEOUT_IN_MS(timerPeriodMs));

        DMF_Rundown_Dereference(moduleContext->DmfModuleRundown);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Timer ABORT RESTART");
    }

Exit:

    return;
}

#pragma code_seg("PAGE")
ScheduledTask_Result_Type
ScheduledTask_ClientWorkDo(
    _In_ DMFMODULE DmfModule,
    _In_ VOID* ClientContext,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
/*++

Routine Description:

    Execute the work the Client wants to perform one time.

Parameters:

    DmfModule - This Module's handle.
    ClientContext - Caller's context.
    PreviousState - Previous power state. Only valid when called from D0Entry.

Return:

    None

--*/
{
    ScheduledTask_Result_Type workResult;
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    // This function can be called with TimerStart == TRUE in case when Ex call
    // happens after a previous call has started a timer.
    //

    switch (moduleConfig->PersistenceType)
    {
        case ScheduledTask_Persistence_PersistentAcrossReboots:
        {
            ULONG timesRun;

            TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_Persistence_PersistentAcrossReboots");

            DMF_ScheduledTask_TimesRunGet(DmfModule,
                                          &timesRun);
            TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "timesRun=%u", timesRun);
            if (timesRun >= 1)
            {
                // In this case the work has been done so don't do it again.
                //
                workResult = ScheduledTask_WorkResult_Success;
                goto Exit;
            }
            break;
        }
        case ScheduledTask_Persistence_NotPersistentAcrossReboots:
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_Persistence_NotPersistentAcrossReboots");
            break;
        }
        default:
        {
            DmfAssert(FALSE);
            break;
        }
    }

    // NOTE: No need to call DMF_Rundown_Reference() here because it is called in ScheduledTask_TimerRestart().
    //       In rare cases, it is possible the callback can run one time after Cancel() has been executed because the 
    //       callback may have started after Timer has been dequeued when Cancel() is called.
    //
    if (moduleContext->WorkIsCompleted)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Work has already been completed...action not taken.");
        workResult = ScheduledTask_WorkResult_Success;
        goto Exit;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "Call EvtScheduledTaskCallback=0x%p", &moduleConfig->EvtScheduledTaskCallback);
    workResult = moduleConfig->EvtScheduledTaskCallback(DmfModule,
                                                        ClientContext,
                                                        PreviousState);
    switch (workResult)
    {
        case ScheduledTask_WorkResult_Success:
        {
            // This is a Write-Only variable. Once set, it is never cleared.
            // It means the Client's callback will never execute again.
            //
            moduleContext->WorkIsCompleted = TRUE;

            // Client's work succeeded. Need to remember not to do work again.
            //
            switch (moduleConfig->PersistenceType)
            {
                case ScheduledTask_Persistence_PersistentAcrossReboots:
                {
                    // Remember across reboots by writing to registry.
                    //
                    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_Persistence_PersistentAcrossReboots Set WorkIsCompleted");
                    DMF_ScheduledTask_TimesRunSet(DmfModule,
                                                  1);
                    break;
                }
                case ScheduledTask_Persistence_NotPersistentAcrossReboots:
                {
                    // Remember for duration of driver load by writing to memory.
                    //
                    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_Persistence_NotPersistentAcrossReboots Set WorkIsCompleted");
                    break;
                }
                default:
                {
                    DmfAssert(FALSE);
                    break;
                }
            }
            break;
        }
        case ScheduledTask_WorkResult_SuccessButTryAgain:
        {
            // This is not ScheduledTask but allows Client to do the operation again.
            // It is basically a timer that allows us to switch easily from timer to Run Once.
            //
            TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_WorkResult_SuccessButTryAgain");
            ScheduledTask_TimerRestart(DmfModule,
                                       workResult);
            break;
        }
        case ScheduledTask_WorkResult_Fail:
        {
            // Client's work failed or Client wants to retry on demand.
            // Client will try again later.
            //
            TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_WorkResult_Fail");
            break;
        }
        case ScheduledTask_WorkResult_FailButTryAgain:
        {
            // Client's work fails: but Client wants to retry.
            //
            TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_WorkResult_FailButTryAgain");

            ScheduledTask_TimerRestart(DmfModule,
                                       workResult);
            break;
        }
        default:
        {
            DmfAssert(FALSE);
            // For SAL.
            //
            break;
        }
    }

Exit:

    FuncExit(DMF_TRACE, "workResult=%d", workResult);

    return workResult;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Function_class_(EVT_WDF_TIMER)
VOID
ScheduledTask_TimerHandler(
    _In_ WDFTIMER WdfTimer
    )
/*++

Routine Description:

    Execute the deferred work the Client wants to perform one time.

Parameters:

    WdfTimer - Timer object that spawns this call.

Return:

    None

--*/
{
    DMFMODULE dmfModule;
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask timer expires");

    dmfModule = (DMFMODULE)WdfTimerGetParentObject(WdfTimer);
    DmfAssert(dmfModule != NULL);

    moduleContext = DMF_CONTEXT_GET(dmfModule);
    moduleConfig = DMF_CONFIG_GET(dmfModule);

    // Timer has executed. Remember this.
    //
    DmfAssert(moduleContext->TimerIsStarted);
    moduleContext->TimerIsStarted = FALSE;

    // Deferred operations do not return the result.
    // If the Client needs the result of the operation, then the deferred option
    // cannot be used.
    //
    ScheduledTask_ClientWorkDo(dmfModule,
                               moduleConfig->CallbackContext,
                               WdfPowerDeviceInvalid);

    FuncExitVoid(DMF_TRACE);
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Function_class_(EVT_WDF_WORKITEM)
VOID
ScheduledTask_DeferredHandlerOnDemand(
    _In_ WDFWORKITEM WdfWorkitem
    )
/*++

Routine Description:

    Execute the On Demand deferred work the Client wants to perform.

Parameters:

    WdfWorkitem - Workitem object that spawns this call.

Return:

    None

--*/
{
    DMFMODULE dmfModule;
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;
    LONG pendingCalls;
    NTSTATUS ntStatus;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    dmfModule = *WdfObjectGet_DMFMODULE(WdfWorkitem);
    moduleContext = DMF_CONTEXT_GET(dmfModule);
    moduleConfig = DMF_CONFIG_GET(dmfModule);

    DmfAssert(moduleContext->NumberOfPendingCalls > 0);
    do
    {
        ntStatus = DMF_Rundown_Reference(moduleContext->DmfModuleRundown);
        if (NT_SUCCESS(ntStatus))
        {
            // Deferred operations do not return the result.
            // If the Client needs the result of the operation, then the deferred option
            // cannot be used.
            //

            // NOTE: OnDemandCallbackContext is only really useful in the case where a single 
            //       call is made.
            //       If multiple calls are made, then the context passed will be for the
            //       first call. (Essentially it is only used to determine if the call
            //       is On Demand or not.)
            //
            TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "Call EvtScheduledTaskCallback=0x%p", &moduleConfig->EvtScheduledTaskCallback);
            if (moduleContext->OnDemandCallbackContext != NULL)
            {
                ScheduledTask_Result_Type workResult;

                workResult = moduleConfig->EvtScheduledTaskCallback(dmfModule,
                                                                    moduleContext->OnDemandCallbackContext,
                                                                    WdfPowerDeviceInvalid);
                // workResult is not honored due to bug in legacy implementation.
                // In order to maintain compatibility with legacy Clients, this behavior is retained.
                // Use Ex version of deferred call for correct behavior which honors the return value.
                //
            }
            else
            {
                // This call honors the Client callback return value.
                //
                ScheduledTask_ClientWorkDo(dmfModule,
                                           moduleConfig->CallbackContext,
                                           WdfPowerDeviceInvalid);
            }

            DMF_Rundown_Dereference(moduleContext->DmfModuleRundown);
        }
        else
        {
            // Let NumberOfPendingCalls decrement to zero.
            //
        }
        pendingCalls = InterlockedDecrement(&moduleContext->NumberOfPendingCalls);
    } while (pendingCalls > 0);

    FuncExitVoid(DMF_TRACE);
}
#pragma code_seg()

///////////////////////////////////////////////////////////////////////////////////////////////////////
// WDF Module Callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

#pragma code_seg("PAGE")
_Function_class_(DMF_ModulePrepareHardware)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DMF_ScheduledTask_ModulePrepareHardware(
    _In_ DMFMODULE DmfModule,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
/*++

Routine Description:

    Called when the Client driver starts. In this case, check if the work should be done
    in PrepareHardware. If so, if it is immediate, do it now. Otherwise, it is deferred. In that
    case start a timer so the work can be done in the timer's handler. If the timer has
    already started don't start it again, because it means that the first iteration of
    work has already been done.

Arguments:

    DmfModule - This Module's handle.
    ResourcesRaw: Same as passed to EvtPrepareHardware.
    ResourcesTranslated: Same as passed to EvtPrepareHardware.

Return Value:

    STATUS_SUCCESS on success, or Error Status Code on failure.

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    ntStatus = STATUS_SUCCESS;

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    switch (moduleConfig->ExecuteWhen)
    {
        case ScheduledTask_ExecuteWhen_PrepareHardware:
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "ScheduledTask_ExecuteWhen_PrepareHardware");
            moduleContext->DisableRetries = FALSE;

            switch (moduleConfig->ExecutionMode)
            {
                case ScheduledTask_ExecutionMode_Deferred:
                {
                    TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "ScheduledTask_ExecutionMode_Deferred");
                    // Only start the timer if the timer has not started.
                    // This allows the SuccessButTryAgain mode to function without
                    // extra initial timer launches.
                    //
                    if (! moduleContext->TimerIsStarted)
                    {
                        // The timer timeout is set to zero so it happens immediately.
                        // (The first iteration happens immediately. After that, the 
                        // retry interval is used.)
                        //
                        TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "Timer START");
                        moduleContext->TimerIsStarted = TRUE;
                        WdfTimerStart(moduleContext->Timer,
                                      WDF_REL_TIMEOUT_IN_MS(moduleConfig->TimeMsBeforeInitialCall));
                    }
                    break;
                }
                case ScheduledTask_ExecutionMode_Immediate:
                {
                    TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "ScheduledTask_ExecutionMode_Immediate");

                    ScheduledTask_Result_Type workResult;

                    workResult = ScheduledTask_ClientWorkDo(DmfModule,
                                                            moduleConfig->CallbackContext,
                                                            WdfPowerDeviceInvalid);
                    if ((workResult != ScheduledTask_WorkResult_Success) &&
                        (workResult != ScheduledTask_WorkResult_SuccessButTryAgain))
                    {
                        ntStatus = STATUS_UNSUCCESSFUL;
                        goto Exit;
                    }
                    break;
                }
                default:
                {
                    DmfAssert(FALSE);
                    break;
                }
            }
        }
    }

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Function_class_(DMF_ModuleReleaseHardware)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DMF_ScheduledTask_ModuleReleaseHardware(
    _In_ DMFMODULE DmfModule,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
/*++

Routine Description:

    When the Config is set to (when "ExecuteWhen == ScheduledTask_ExecuteWhen_PrepareHardware):
    Prevent timer from being restarted. It will be restarted on PrepareHardware if necessary.
    This callback disables any timers that will be restarted during PrepareHardware because
    PrepareHardware attempts to execute the associated callback during every call
    (when "ExecuteWhen==ScheduledTask_ExecuteWhen_PrepareHardware).

Arguments:

    DmfModule - The given DMF Module.

Return Value:

    STATUS_SUCCESS

--*/
{
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "ReleaseHardware: Set DisableRetries");

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    if (moduleConfig->ExecuteWhen == ScheduledTask_ExecuteWhen_PrepareHardware)
    {
        // Disable timer to prevent retries if possible.
        //
        WdfTimerStop(moduleContext->Timer,
                     FALSE);
        // Try to prevent retries if handler is running.
        //
        moduleContext->DisableRetries = TRUE;
    }

    FuncExitVoid(DMF_TRACE);

    return STATUS_SUCCESS;
}
#pragma code_seg()

_Function_class_(DMF_ModuleD0Entry)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DMF_ScheduledTask_ModuleD0Entry(
    _In_ DMFMODULE DmfModule,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
/*++

Routine Description:

    Called when this Module powers up. In this case, check if the work should be done
    in D0Entry. If so, if it is immediate, do it now. Otherwise, it is deferred. In that
    case start a timer so the work can be done in the timer's handler. If the timer has
    already started don't start it again, because it means that the first iteration of
    work has already been done.

Arguments:

    DmfModule - The given DMF Module.
    PreviousState - The WDF Power State that the given DMF Module should exit from.

Return Value:

    STATUS_SUCCESS

--*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    FuncEntry(DMF_TRACE);

    UNREFERENCED_PARAMETER(PreviousState);

    ntStatus = STATUS_SUCCESS;

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    moduleConfig = DMF_CONFIG_GET(DmfModule);

    switch (moduleConfig->ExecuteWhen)
    {
        case ScheduledTask_ExecuteWhen_D0Entry:
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_ExecuteWhen_D0Entry");
            moduleContext->DisableRetries = FALSE;

            switch (moduleConfig->ExecutionMode)
            {
                case ScheduledTask_ExecutionMode_Deferred:
                {
                    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_ExecutionMode_Deferred");
                    // Only start the timer if the timer has not started.
                    // This allows the SuccessButTryAgain mode to function without
                    // extra initial timer launches.
                    //
                    if (! moduleContext->TimerIsStarted)
                    {
                        // The timer timeout is set to zero so it happens immediately.
                        // (The first iteration happens immediately. After that, the 
                        // retry interval is used.)
                        //
                        TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Timer START");
                        moduleContext->TimerIsStarted = TRUE;
                        WdfTimerStart(moduleContext->Timer,
                                      WDF_REL_TIMEOUT_IN_MS(moduleConfig->TimeMsBeforeInitialCall));
                    }
                    break;
                }
                case ScheduledTask_ExecutionMode_Immediate:
                {
                    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ScheduledTask_ExecutionMode_Immediate");

                    ScheduledTask_Result_Type workResult;

                    // Do the work now, in D0Entry.
                    //
                    workResult = ScheduledTask_ClientWorkDo(DmfModule,
                                                            moduleConfig->CallbackContext,
                                                            PreviousState);
                    if ((workResult != ScheduledTask_WorkResult_Success) &&
                        (workResult != ScheduledTask_WorkResult_SuccessButTryAgain))
                    {
                        ntStatus = STATUS_UNSUCCESSFUL;
                        goto Exit;
                    }
                    break;
                }
                default:
                {
                    DmfAssert(FALSE);
                    break;
                }
            }
        }
    }

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}

_Function_class_(DMF_ModuleD0Exit)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DMF_ScheduledTask_ModuleD0Exit(
    _In_ DMFMODULE DmfModule,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
/*++

Routine Description:

    When the Config is set to (when "ExecuteWhen == ScheduledTask_ExecuteWhen_D0Exit):
    Prevent timer from being restarted. It will be restarted on D0Entry if necessary.
    This callback disables any timers that will be restarted during D0Exit because
    PrepareHardware attempts to execute the associated callback during every call
    (when "ExecuteWhen==ScheduledTask_ExecuteWhen_D0Entry).

Arguments:

    DmfModule - This Module's handle.
    TargetState - The WDF Power State that the given DMF Module will enter.

Return Value:

    STATUS_SUCCESS

--*/
{
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    UNREFERENCED_PARAMETER(TargetState);

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    if (moduleConfig->ExecuteWhen == ScheduledTask_ExecuteWhen_D0Entry)
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, DMF_TRACE, "D0Exit: Set DisableRetries");
        // Disable timer to prevent retries if possible.
        //
        WdfTimerStop(moduleContext->Timer,
                     FALSE);
        // Try to prevent retries if handler is running.
        //
        moduleContext->DisableRetries = TRUE;
    }

    FuncExitVoid(DMF_TRACE);

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////
// DMF Module Callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

#pragma code_seg("PAGE")
_Function_class_(DMF_Open)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static
NTSTATUS
DMF_ScheduledTask_Open(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Initialize an instance of a DMF Module of type ScheduledTask.

Arguments:

    DmfModule - This Module's handle.

Return Value:

    NT_STATUS code indicating success or failure.

--*/
{
    NTSTATUS ntStatus;
    WDF_OBJECT_ATTRIBUTES objectAttributes;
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;
    WDF_TIMER_CONFIG timerConfig;
    WDF_WORKITEM_CONFIG workitemConfig;
    WDFDEVICE device;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);
    device = DMF_ParentDeviceGet(DmfModule);
    ntStatus = STATUS_SUCCESS;

    moduleContext->TimerIsStarted = FALSE;
    moduleContext->NumberOfPendingCalls = 0;
    moduleContext->DisableRetries = FALSE;

    // Create a timer so that the run once callback can be executed in deferred mode.
    // NOTE: Deferred calls can happen in immediate mode when callback returns a retry.
    //
    WDF_TIMER_CONFIG_INIT(&timerConfig,
                          ScheduledTask_TimerHandler);
    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    objectAttributes.ParentObject = DmfModule;
    objectAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    ntStatus = WdfTimerCreate(&timerConfig,
                              &objectAttributes,
                              &moduleContext->Timer);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfTimerCreate fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

    // Create a workitem for possible on demand calls.
    //
    WDF_WORKITEM_CONFIG_INIT(&workitemConfig,
                             ScheduledTask_DeferredHandlerOnDemand);

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&objectAttributes,
                                           DMFMODULE);

    // Use WdfDevice instead of DmfModule as a parent, so that the work item is not disposed 
    // prematurely when this Module is deleted as a part of a Dynamic Module tree.
    //
    objectAttributes.ParentObject = device;

    ntStatus = WdfWorkItemCreate(&workitemConfig,
                                 &objectAttributes,
                                 &moduleContext->DeferredOnDemand);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfTimerCreate fails: ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

    DMF_ModuleInContextSave(moduleContext->DeferredOnDemand,
                            DmfModule);

    DMF_Rundown_Start(moduleContext->DmfModuleRundown);

Exit:

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Function_class_(DMF_Close)
_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
DMF_ScheduledTask_Close(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Uninitialize an instance of a DMF Module of type ScheduledTask.

Arguments:

    DmfModule - This Module's handle.

Return Value:

    None

--*/
{
    DMF_CONTEXT_ScheduledTask* moduleContext;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);
    moduleConfig = DMF_CONFIG_GET(DmfModule);

    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "ModuleClosing");

    DMF_ScheduledTask_Cancel(DmfModule);

    DmfAssert(0 == moduleContext->NumberOfPendingCalls);

    WdfObjectDelete(moduleContext->DeferredOnDemand);
    moduleContext->DeferredOnDemand = NULL;

    WdfObjectDelete(moduleContext->Timer);
    moduleContext->Timer = NULL;
    moduleContext->TimerIsStarted = FALSE;

    FuncExitNoReturn(DMF_TRACE);
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Function_class_(DMF_ChildModulesAdd)
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
DMF_ScheduledTask_ChildModulesAdd(
    _In_ DMFMODULE DmfModule,
    _In_ DMF_MODULE_ATTRIBUTES* DmfParentModuleAttributes,
    _In_ PDMFMODULE_INIT DmfModuleInit
    )
/*++

Routine Description:

    Configure and add the required Child Modules to the given Parent Module.

Arguments:

    DmfModule - The given Parent Module.
    DmfParentModuleAttributes - Pointer to the parent DMF_MODULE_ATTRIBUTES structure.
    DmfModuleInit - Opaque structure to be passed to DMF_DmfModuleAdd.

Return Value:

    None

--*/
{
    DMF_MODULE_ATTRIBUTES moduleAttributes;
    DMF_CONTEXT_ScheduledTask* moduleContext;

    UNREFERENCED_PARAMETER(DmfParentModuleAttributes);
    UNREFERENCED_PARAMETER(DmfModuleInit);

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // Rundown
    // -------
    //
    DMF_Rundown_ATTRIBUTES_INIT(&moduleAttributes);
    DMF_DmfModuleAdd(DmfModuleInit,
                     &moduleAttributes,
                     WDF_NO_OBJECT_ATTRIBUTES,
                     &moduleContext->DmfModuleRundown);

    FuncExitVoid(DMF_TRACE);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////
// Public Calls by Client
///////////////////////////////////////////////////////////////////////////////////////////////////////
//

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_ScheduledTask_Create(
    _In_ WDFDEVICE Device,
    _In_ DMF_MODULE_ATTRIBUTES* DmfModuleAttributes,
    _In_ WDF_OBJECT_ATTRIBUTES* ObjectAttributes,
    _Out_ DMFMODULE* DmfModule
    )
/*++

Routine Description:

    Create an instance of a DMF Module of type ScheduledTask.

Arguments:

    Device - Client driver's WDFDEVICE object.
    DmfModuleAttributes - Opaque structure that contains parameters DMF needs to initialize the Module.
    ObjectAttributes - WDF object attributes for DMFMODULE.
    DmfModule - Address of the location where the created DMFMODULE handle is returned.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS ntStatus;
    DMF_MODULE_DESCRIPTOR dmfModuleDescriptor_ScheduledTask;
    DMF_CALLBACKS_DMF dmfCallbacksDmf_ScheduledTask;
    DMF_CALLBACKS_WDF dmfCallbacksWdf_ScheduledTask;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleConfig = (DMF_CONFIG_ScheduledTask*)DmfModuleAttributes->ModuleConfigPointer;

    DMF_CALLBACKS_DMF_INIT(&dmfCallbacksDmf_ScheduledTask);
    dmfCallbacksDmf_ScheduledTask.ChildModulesAdd = DMF_ScheduledTask_ChildModulesAdd;
    dmfCallbacksDmf_ScheduledTask.DeviceOpen = DMF_ScheduledTask_Open;
    dmfCallbacksDmf_ScheduledTask.DeviceClose = DMF_ScheduledTask_Close;

    // Allow Module to be created Dynamically when possible.
    //
    if ((moduleConfig->ExecuteWhen == ScheduledTask_ExecuteWhen_PrepareHardware) ||
        (moduleConfig->ExecuteWhen == ScheduledTask_ExecuteWhen_D0Entry))
    {
        DMF_CALLBACKS_WDF_INIT(&dmfCallbacksWdf_ScheduledTask);
        dmfCallbacksWdf_ScheduledTask.ModulePrepareHardware = DMF_ScheduledTask_ModulePrepareHardware;
        dmfCallbacksWdf_ScheduledTask.ModuleReleaseHardware = DMF_ScheduledTask_ModuleReleaseHardware;
        dmfCallbacksWdf_ScheduledTask.ModuleD0Entry = DMF_ScheduledTask_ModuleD0Entry;
        dmfCallbacksWdf_ScheduledTask.ModuleD0Exit = DMF_ScheduledTask_ModuleD0Exit;
    }

    DMF_MODULE_DESCRIPTOR_INIT_CONTEXT_TYPE(dmfModuleDescriptor_ScheduledTask,
                                            ScheduledTask,
                                            DMF_CONTEXT_ScheduledTask,
                                            DMF_MODULE_OPTIONS_PASSIVE,
                                            DMF_MODULE_OPEN_OPTION_OPEN_Create);

    dmfModuleDescriptor_ScheduledTask.CallbacksDmf = &dmfCallbacksDmf_ScheduledTask;
    // Allow Module to be created Dynamically when possible.
    //
    if ((moduleConfig->ExecuteWhen == ScheduledTask_ExecuteWhen_PrepareHardware) ||
        (moduleConfig->ExecuteWhen == ScheduledTask_ExecuteWhen_D0Entry))
    {
        dmfModuleDescriptor_ScheduledTask.CallbacksWdf = &dmfCallbacksWdf_ScheduledTask;
    }

    ntStatus = DMF_ModuleCreate(Device,
                                DmfModuleAttributes,
                                ObjectAttributes,
                                &dmfModuleDescriptor_ScheduledTask,
                                DmfModule);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "DMF_ModuleCreate fails: ntStatus=%!STATUS!", ntStatus);
    }

    FuncExit(DMF_TRACE, "ntStatus=%!STATUS!", ntStatus);

    return(ntStatus);
}
#pragma code_seg()

// Module Methods
//

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
DMF_ScheduledTask_Cancel(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Cancel Ongoing ScheduledTask Execution.

Arguments:

    DmfModule - This Module's handle.

Return Value:

    None

++*/
{
    DMF_CONTEXT_ScheduledTask* moduleContext;

    PAGED_CODE();

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DMFMODULE_VALIDATE_IN_METHOD_CLOSING_OK(DmfModule,
                                            ScheduledTask);

    // If a callback starts here, this Method waits for the callback to finish here.
    //
    DMF_Rundown_EndAndWait(moduleContext->DmfModuleRundown);

    // If a callback starts here, the callback will do nothing because of above call.
    //
 
    // Wait for any On Demand calls to finish.
    //
    WdfWorkItemFlush(moduleContext->DeferredOnDemand);
    DmfAssert(0 == moduleContext->NumberOfPendingCalls);

    // Stop the timer and wait for any pending call to finish.
    //
    WdfTimerStop(moduleContext->Timer,
                 TRUE);
    moduleContext->TimerIsStarted = FALSE;

    FuncExitNoReturn(DMF_TRACE);
}
#pragma code_seg()

#pragma code_seg("PAGE")
_Must_inspect_result_
ScheduledTask_Result_Type
DMF_ScheduledTask_ExecuteNow(
    _In_ DMFMODULE DmfModule,
    _In_ VOID* CallbackContext
    )
/*++

Routine Description:

    Execute the associated ScheduledTask handler immediately (synchronously).

Arguments:

    DmfModule - This Module's handle.
    CallbackContext - Context pass as ModuleContext to ScheduledTask handler.

Return Value:

    STATUS_SUCCESS - The deferred call was launched.
    STATUS_UNSUCCESSFUL - The deferred call was not launched because Module is closing.

++*/
{
    ScheduledTask_Result_Type workResult;
    DMF_CONFIG_ScheduledTask* moduleConfig;

    PAGED_CODE();

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 ScheduledTask);

    moduleConfig = DMF_CONFIG_GET(DmfModule);

    // NOTE: Caller probably needs to lock because deferred execution may be pending.
    //
    workResult = moduleConfig->EvtScheduledTaskCallback(DmfModule,
                                                        CallbackContext,
                                                        WdfPowerDeviceInvalid);

    return workResult;
}
#pragma code_seg()

_Must_inspect_result_
NTSTATUS
DMF_ScheduledTask_ExecuteNowDeferred(
    _In_ DMFMODULE DmfModule,
    _In_opt_ VOID* CallbackContext
    )
/*++

Routine Description:

    Executes the associated ScheduledTask callback in a deferred manner but does
    NOT honor the callback's return value due to bug in original implementation.
    This Method is included for legacy Clients only. Use the "Ex" version instead.

Arguments:

    DmfModule - This Module's handle.
    CallbackContext - Context passed to ScheduledTask handler.

Return Value:

    STATUS_SUCCESS - The deferred call was launched.
    STATUS_UNSUCCESSFUL - The deferred call was not launched due to cancellation.

++*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_ScheduledTask* moduleContext;

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 ScheduledTask);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    // If the workitem has already been enqueued, just increment the number of times the
    // ScheduledTask routine must be called.
    //
    if (1 == InterlockedIncrement(&moduleContext->NumberOfPendingCalls))
    {
        ntStatus = DMF_Rundown_Reference(moduleContext->DmfModuleRundown);
        if (NT_SUCCESS(ntStatus))
        {
            // Enqueue the workitem for the first call.
            // NOTE: This context is only really useful in the case where a single call is made.
            //       If multiple calls are made, then the context passed will be for the
            //       first call. (Essentially it is only used to determine if the call
            //       is On Demand or not.)
            //
            moduleContext->OnDemandCallbackContext = CallbackContext;
            WdfWorkItemEnqueue(moduleContext->DeferredOnDemand);
            ntStatus = STATUS_SUCCESS;
            DMF_Rundown_Dereference(moduleContext->DmfModuleRundown);
        }
        else
        {
            ntStatus = STATUS_UNSUCCESSFUL;
            InterlockedDecrement(&moduleContext->NumberOfPendingCalls);
        }
    }
    else
    {
        // There is already a workitem enqueued. The routine will execute.
        //
        ntStatus = STATUS_SUCCESS;
    }

    return ntStatus;
}

_Must_inspect_result_
NTSTATUS
DMF_ScheduledTask_ExecuteNowDeferredEx(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Executes the associated ScheduledTask callback in a deferred manner and honors
    the callback's return value. The callback is passed the context specified in Module Config.

Arguments:

    DmfModule - This Module's handle.

Return Value:

    STATUS_SUCCESS - The deferred call was launched.
    STATUS_UNSUCCESSFUL - The deferred call was not launched due to cancellation.

++*/
{
    NTSTATUS ntStatus;
    DMF_CONTEXT_ScheduledTask* moduleContext;

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 ScheduledTask);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DmfAssert(moduleContext->OnDemandCallbackContext == NULL);

    // If the workitem has already been enqueued, just increment the number of times the
    // ScheduledTask routine must be called.
    //
    if (1 == InterlockedIncrement(&moduleContext->NumberOfPendingCalls))
    {
        ntStatus = DMF_Rundown_Reference(moduleContext->DmfModuleRundown);
        if (NT_SUCCESS(ntStatus))
        {
            moduleContext->OnDemandCallbackContext = NULL;
            WdfWorkItemEnqueue(moduleContext->DeferredOnDemand);
            ntStatus = STATUS_SUCCESS;
            DMF_Rundown_Dereference(moduleContext->DmfModuleRundown);
        }
        else
        {
            ntStatus = STATUS_UNSUCCESSFUL;
            InterlockedDecrement(&moduleContext->NumberOfPendingCalls);
        }
    }
    else
    {
        // There is already a workitem enqueued. The routine will execute.
        //
        ntStatus = STATUS_SUCCESS;
    }

    return ntStatus;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
DMF_ScheduledTask_Restart(
    _In_ DMFMODULE DmfModule
    )
/*++

Routine Description:

    Call this Method to enable execution of task handler after calling DMF_ScheduledTask_Cancel.

Arguments:

    DmfModule - This Module's handle.

Return Value:

    None

++*/
{
    DMF_CONTEXT_ScheduledTask* moduleContext;

    FuncEntry(DMF_TRACE);

    moduleContext = DMF_CONTEXT_GET(DmfModule);

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 ScheduledTask);

    DMF_Rundown_Start(moduleContext->DmfModuleRundown);

    FuncExitNoReturn(DMF_TRACE);
}

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_ScheduledTask_TimesRunGet(
    _In_ DMFMODULE DmfModule,
    _Out_ ULONG* TimesRun
    )
/*++

Routine Description:

    Reads the default variable from the registry.

Arguments:

    DmfModule - This Module's handle.
    TimesRun - Value that is read.

Return Value:

    None

++*/
{
    NTSTATUS ntStatus;
    WDFKEY wdfKey;
    WDFDEVICE device;
    WDFDRIVER driver;
    ULONG value;
    UNICODE_STRING valueNameString;

    PAGED_CODE();

    DmfAssert(TimesRun != NULL);
    *TimesRun = 0;

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 ScheduledTask);

    wdfKey = NULL;
    device = DMF_ParentDeviceGet(DmfModule);
    driver = WdfDeviceGetDriver(device);

    // KEY_READ is OK for both Kernel-mode and User-mode.
    //
    ntStatus = WdfDriverOpenParametersRegistryKey(driver,
                                                  KEY_READ,
                                                  WDF_NO_OBJECT_ATTRIBUTES,
                                                  &wdfKey);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfDriverOpenParametersRegistryKey ntStatus=%!STATUS!", ntStatus);
        wdfKey = NULL;
        goto Exit;
    }

    RtlInitUnicodeString(&valueNameString,
                         DEFAULT_NAME_DEVICE);
    ntStatus = WdfRegistryQueryValue(wdfKey,
                                     &valueNameString,
                                     sizeof(DWORD),
                                     &value,
                                     NULL,
                                     NULL);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfRegistryQueryValue ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

    *TimesRun = value;
    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Read TimesRun=%u", *TimesRun);

Exit:

    if (wdfKey != NULL)
    {
        WdfRegistryClose(wdfKey);
        wdfKey = NULL;
    }

    return ntStatus;
}
#pragma code_seg()

#pragma code_seg("PAGE")
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
DMF_ScheduledTask_TimesRunSet(
    _In_ DMFMODULE DmfModule,
    _In_ ULONG TimesRun
    )
/*++

Routine Description:

    Writes the default variable into the registry.

Arguments:

    DmfModule - This Module's handle.
    TimesRun - Value to write.

Return Value:

    None

++*/
{
    NTSTATUS ntStatus;
    WDFKEY wdfKey;
    WDFDEVICE device;
    WDFDRIVER driver;
    UNICODE_STRING valueNameString;
    ACCESS_MASK accessMask;

    PAGED_CODE();

    DMFMODULE_VALIDATE_IN_METHOD(DmfModule,
                                 ScheduledTask);

    wdfKey = NULL;
    device = DMF_ParentDeviceGet(DmfModule);
    driver = WdfDeviceGetDriver(device);

#if !defined(DMF_USER_MODE)
    accessMask = KEY_WRITE;
#else
    accessMask = KEY_SET_VALUE;
#endif // !defined(DMF_USER_MODE)

    ntStatus = WdfDriverOpenParametersRegistryKey(driver,
                                                  accessMask,
                                                  WDF_NO_OBJECT_ATTRIBUTES,
                                                  &wdfKey);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfDriverOpenParametersRegistryKey ntStatus=%!STATUS!", ntStatus);
        wdfKey = NULL;
        goto Exit;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DMF_TRACE, "Write TimesRun=%d", TimesRun);

    RtlInitUnicodeString(&valueNameString,
                         DEFAULT_NAME_DEVICE);
    ntStatus = WdfRegistryAssignULong(wdfKey,
                                      &valueNameString,
                                      TimesRun);
    if (! NT_SUCCESS(ntStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DMF_TRACE, "WdfRegistryAssignULong ntStatus=%!STATUS!", ntStatus);
        goto Exit;
    }

Exit:

    if (wdfKey != NULL)
    {
        WdfRegistryClose(wdfKey);
        wdfKey = NULL;
    }

    return ntStatus;
}
#pragma code_seg()

// eof: Dmf_ScheduledTask.c
//
