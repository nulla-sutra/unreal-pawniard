// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "LedgeSafeWalkingMode.h"

#include "AI/Navigation/NavigationDataInterface.h"
#include "DefaultMovementSet/Modes/SmoothWalkingMode.h"
#include "DefaultMovementSet/NavMoverComponent.h"
#include "DefaultMovementSet/Settings/CommonLegacyMovementSettings.h"
#include "Engine/ScopedMovementUpdate.h"
#include "MoverComponent.h"

namespace
{
    // Allow sub-centimeter differences between collision movement and Recast's surface result.
    constexpr float NavLocationTolerance = 0.1f;
}

ULedgeSafeWalkingMode::ULedgeSafeWalkingMode()
{
    bSlideAlongNavMeshEdge = true;

    // SmoothWalking's constructor is not exported; reflected construction reuses it without private engine APIs.
    MoveGenerationSettings = CastChecked<UWalkingMode>(CreateDefaultSubobject(TEXT("MoveGenerationSettings"),
        UWalkingMode::StaticClass(), USmoothWalkingMode::StaticClass(), true, false));
}

void ULedgeSafeWalkingMode::OnRegistered(FName ModeName, const FMoverSimContext& SimContext)
{
    Super::OnRegistered(ModeName, SimContext);

    if (ActiveWalkingMode)
    {
        ActiveWalkingMode->OnUnregistered(SimContext);
        ActiveWalkingMode = nullptr;
    }

    if (ensureMsgf(MoveGenerationSettings, TEXT("LedgeSafeWalkingMode requires its SmoothWalking settings template.")))
    {
        // Movement modes resolve their Mover through their immediate outer, so the template cannot simulate directly.
        ActiveWalkingMode = NewObject<UWalkingMode>(GetOuter(), MoveGenerationSettings->GetClass(), NAME_None,
            RF_Transient, MoveGenerationSettings.Get());
        ActiveWalkingMode->OnRegistered(ModeName, SimContext);
    }
}

void ULedgeSafeWalkingMode::OnUnregistered(const FMoverSimContext& SimContext)
{
    if (ActiveWalkingMode)
    {
        ActiveWalkingMode->OnUnregistered(SimContext);
        ActiveWalkingMode = nullptr;
    }

    Super::OnUnregistered(SimContext);
}

void ULedgeSafeWalkingMode::Activate(const FMoverEventContext& Context, FName PrevModeName,
                                    const FMoverSimContext& SimContext, const FMoverTickStartData& StartState,
                                    FMoverSyncState* OutSyncState, FMoverAuxStateContext* OutAuxState)
{
    // NavWalking activation disables world collision; only use it for navigation queries.
    UNavWalkingMode::Activate(Context, PrevModeName, SimContext, StartState, OutSyncState, OutAuxState);
    NavDataInterface = GetNavData();
    if (ActiveWalkingMode)
    {
        ActiveWalkingMode->Activate(Context, PrevModeName, SimContext, StartState, OutSyncState, OutAuxState);
    }
}

void ULedgeSafeWalkingMode::Deactivate(const FMoverEventContext& Context, FName NextModeName,
                                      const FMoverSimContext& SimContext)
{
    if (ActiveWalkingMode)
    {
        ActiveWalkingMode->Deactivate(Context, NextModeName, SimContext);
    }
    UNavWalkingMode::Deactivate(Context, NextModeName, SimContext);
}

void ULedgeSafeWalkingMode::GenerateMove_Implementation(const FMoverSimContext& SimContext,
                                                     const FMoverTickStartData& StartState,
                                                     const FMoverTimeStep& TimeStep,
                                                     FProposedMove& OutProposedMove) const
{
    if (ActiveWalkingMode)
    {
        ActiveWalkingMode->GenerateMove(SimContext, StartState, TimeStep, OutProposedMove);
    }
    else
    {
        OutProposedMove = FProposedMove();
    }
}

void ULedgeSafeWalkingMode::SimulationTick_Implementation(const FSimulationTickParams& Params,
                                                       FMoverTickEndData& OutputState)
{
    const auto* Mover = GetMoverComponent();
    auto* UpdatedComponent = Params.MovingComps.UpdatedComponent.Get();
    auto* Blackboard = Mover ? Mover->GetSimBlackboard_Mutable() : nullptr;
    const auto DeltaSeconds = Params.TimeStep.StepMs * 0.001f;
    const auto* StartingSyncState = Params.StartState.SyncState.SyncStateCollection.FindDataByType<FMoverDefaultSyncState>();
    if (!Mover || !UpdatedComponent || !Params.MovingComps.UpdatedPrimitive.IsValid() || !Blackboard ||
        !ActiveWalkingMode || !CommonLegacySettings || !StartingSyncState || DeltaSeconds <= UE_SMALL_NUMBER)
    {
        return;
    }

    if (!NavDataInterface.IsValid())
    {
        NavDataInterface = GetNavData();
    }

    const auto* NavData = NavDataInterface.Get();
    const auto UpDirection = Mover->GetUpDirection();
    const auto StartingLocation = UpdatedComponent->GetComponentLocation();
    const auto StartingFeetLocation = NavMoverComponent ? NavMoverComponent->GetFeetLocation() : StartingLocation;
    FNavLocation StartingNavLocation;
    // Projection can find a nearby polygon even when the pawn is outside it; never snap onto that polygon.
    const auto bHasNavFloor = NavMoverComponent && NavData &&
        FindNavFloor(StartingFeetLocation, StartingNavLocation, NavData) &&
        NavData->IsNodeRefValid(StartingNavLocation.NodeRef) &&
        FVector::VectorPlaneProject(StartingNavLocation.Location - StartingFeetLocation, UpDirection)
            .IsNearlyZero(NavLocationTolerance);

    // Mover data collections deep-copy through assignment; copy construction would share the trial's data blocks.
    FSimulationTickParams WalkingParams;
    WalkingParams = Params;
    const auto DesiredPlanarDelta = FVector::VectorPlaneProject(Params.ProposedMove.LinearVelocity * DeltaSeconds,
                                                               UpDirection);
    FNavLocation TargetNavLocation;
    if (bHasNavFloor && NavData->FindMoveAlongSurface(StartingNavLocation,
        StartingNavLocation.Location + DesiredPlanarDelta, TargetNavLocation) &&
        NavData->IsNodeRefValid(TargetNavLocation.NodeRef))
    {
        const auto AllowedPlanarDelta = FVector::VectorPlaneProject(TargetNavLocation.Location - StartingFeetLocation,
                                                                   UpDirection);
        if (!bSlideAlongNavMeshEdge && !AllowedPlanarDelta.Equals(DesiredPlanarDelta, NavLocationTolerance))
        {
            WalkingParams.ProposedMove.LinearVelocity = FVector::ZeroVector;
        }
        else if (!DesiredPlanarDelta.IsNearlyZero())
        {
            // Restrict horizontal travel only; native SmoothWalking owns height, slope and step-up handling.
            WalkingParams.ProposedMove.LinearVelocity = AllowedPlanarDelta / DeltaSeconds +
                UpDirection * Params.ProposedMove.LinearVelocity.Dot(UpDirection);
        }
    }
    else
    {
        WalkingParams.ProposedMove.LinearVelocity = FVector::ZeroVector;
    }

    auto RollbackBlackboard = Mover->GetRollbackBlackboardExternal();
    auto StartingBasedTransformDelta = FTransform::Identity;
    RollbackBlackboard.TryGet(CommonBlackboard::AccumulatedBasedTransformDelta, StartingBasedTransformDelta);
    const auto DiscardTrialCaches = [&]
    {
        // Native idle simulation rebuilds these at the reverted position instead of using the rejected destination.
        Blackboard->Invalidate(CommonBlackboard::LastFloorResult);
        Blackboard->Invalidate(CommonBlackboard::LastFoundDynamicMovementBase);
        RollbackBlackboard.TrySet(CommonBlackboard::AccumulatedBasedTransformDelta, StartingBasedTransformDelta);
    };

    // Even a zero-velocity tick can move through depenetration or based movement, so validate both attempts.
    FScopedMovementUpdate ScopedMove(UpdatedComponent, EScopedUpdate::DeferredUpdates);
    FMoverTickEndData WalkingOutput;
    const auto NumAttempts = WalkingParams.ProposedMove.LinearVelocity.IsNearlyZero() ? 1 : 2;
    for (auto Attempt = 0; Attempt < NumAttempts; ++Attempt)
    {
        WalkingOutput = OutputState;
        ActiveWalkingMode->SimulationTick(WalkingParams, WalkingOutput);
        FNavLocation FinalNavLocation;
        const auto FinalFeetLocation = NavMoverComponent ? NavMoverComponent->GetFeetLocation()
                                                        : UpdatedComponent->GetComponentLocation();
        const auto bWithinBoundary = bHasNavFloor
            ? NavData->FindMoveAlongSurface(StartingNavLocation, FinalFeetLocation, FinalNavLocation) &&
              NavData->IsNodeRefValid(FinalNavLocation.NodeRef) &&
              FVector::VectorPlaneProject(FinalNavLocation.Location - FinalFeetLocation, UpDirection)
                  .IsNearlyZero(NavLocationTolerance)
            : FVector::VectorPlaneProject(UpdatedComponent->GetComponentLocation() - StartingLocation, UpDirection)
                  .IsNearlyZero(UE_KINDA_SMALL_NUMBER);
        if (bWithinBoundary)
        {
            CachedNavLocation = FinalNavLocation;
            OutputState = MoveTemp(WalkingOutput);
            return;
        }

        ScopedMove.RevertMove();
        DiscardTrialCaches();
        WalkingParams.ProposedMove.LinearVelocity = FVector::ZeroVector;
    }

    // If native idle movement also crossed the boundary, synchronize its output with the reverted pose.
    OutputState = MoveTemp(WalkingOutput);
    auto& OutputSyncState = OutputState.SyncState.SyncStateCollection.FindOrAddMutableDataByType<FMoverDefaultSyncState>();
    OutputSyncState = *StartingSyncState;
    OutputSyncState.MoveDirectionIntent = FVector::ZeroVector;
    OutputSyncState.SetTransforms_WorldSpace(UpdatedComponent->GetComponentLocation(), UpdatedComponent->GetComponentRotation(),
        FVector::ZeroVector, FVector::ZeroVector, StartingSyncState->GetMovementBase(),
        StartingSyncState->GetMovementBaseBoneName());
    UpdatedComponent->ComponentVelocity = FVector::ZeroVector;
    OutputState.MovementEndState.ResetToDefaults();
    CachedNavLocation = bHasNavFloor ? StartingNavLocation : FNavLocation();
}
