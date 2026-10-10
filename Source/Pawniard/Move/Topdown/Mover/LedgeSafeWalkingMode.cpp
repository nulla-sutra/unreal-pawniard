// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "LedgeSafeWalkingMode.h"

#include "AI/Navigation/NavigationDataInterface.h"
#include "DefaultMovementSet/Modes/SmoothWalkingMode.h"
#include "DefaultMovementSet/NavMoverComponent.h"
#include "DefaultMovementSet/Settings/CommonLegacyMovementSettings.h"
#include "Engine/ScopedMovementUpdate.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MoverComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "PawniardMovementSettings.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr float NavLocationTolerance = 0.1f;
}

ULedgeSafeWalkingMode::ULedgeSafeWalkingMode()
{
    SharedSettingsClasses.Add(UPawniardMovementSettings::StaticClass());
    GameplayTags.AddTag(Mover_IsOnGround);
}

void ULedgeSafeWalkingMode::OnRegistered(FName ModeName, const FMoverSimContext& SimContext)
{
    Super::OnRegistered(ModeName, SimContext);
    UnregisterNativeMode(SimContext);

    auto* Mover = GetMoverComponent();
    const auto* World = Mover ? Mover->GetWorld() : nullptr;
    // Never put derived native settings into an editor asset's authored array.
    if (!World || !World->IsGameWorld())
    {
        return;
    }

    MovementSettings = Mover->FindSharedSettings_Mutable<UPawniardMovementSettings>();
    if (const auto* Owner = Mover->GetOwner())
    {
        NavMoverComponent = Owner->FindComponentByClass<UNavMoverComponent>();
    }
    if (!ensureMsgf(MovementSettings && NavMoverComponent.IsValid(),
        TEXT("LedgeSafeWalkingMode requires PawniardMovementSettings and a NavMoverComponent.")))
    {
        return;
    }

    const auto* ArrayProperty = FindFProperty<FArrayProperty>(UMoverComponent::StaticClass(), TEXT("SharedSettings"));
    const auto* ObjectProperty = ArrayProperty ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
    if (!ensureMsgf(ObjectProperty, TEXT("Mover's shared-settings storage is incompatible with SmoothWalking registration.")))
    {
        return;
    }

    NativeSettings = NewObject<UCommonLegacyMovementSettings>(this, NAME_None, RF_Transient);
    // Native modes require Mover as their immediate outer. Only the wrapper is registered in MovementModes.
    SmoothMode = NewObject<UBaseMovementMode>(Mover, USmoothWalkingMode::StaticClass(), NAME_None, RF_Transient);
    if (!ApplySettings())
    {
        SmoothMode = nullptr;
        NativeSettings = nullptr;
        return;
    }

    // Walking caches this view, while NavMover and based movement query the array on demand.
    // Keep it first for native lookups throughout registration, and remove only this object on unregistration.
    FScriptArrayHelper SharedSettings(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Mover));
    SharedSettings.InsertValues(0);
    ObjectProperty->SetObjectPropertyValue(SharedSettings.GetRawPtr(0), NativeSettings.Get());
    SmoothMode->OnRegistered(ModeName, SimContext);
}

void ULedgeSafeWalkingMode::OnUnregistered(const FMoverSimContext& SimContext)
{
    UnregisterNativeMode(SimContext);
    Super::OnUnregistered(SimContext);
}

void ULedgeSafeWalkingMode::UnregisterNativeMode(const FMoverSimContext& SimContext)
{
    if (SmoothMode)
    {
        SmoothMode->OnUnregistered(SimContext);
        SmoothMode = nullptr;
    }
    if (auto* Mover = GetMoverComponent(); NativeSettings && Mover)
    {
        const auto* ArrayProperty = FindFProperty<FArrayProperty>(UMoverComponent::StaticClass(), TEXT("SharedSettings"));
        const auto* ObjectProperty = ArrayProperty ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
        if (ensure(ObjectProperty))
        {
            FScriptArrayHelper SharedSettings(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Mover));
            for (auto Index = SharedSettings.Num() - 1; Index >= 0; --Index)
            {
                if (ObjectProperty->GetObjectPropertyValue(SharedSettings.GetRawPtr(Index)) == NativeSettings.Get())
                {
                    SharedSettings.RemoveValues(Index);
                }
            }
        }
    }
    NativeSettings = nullptr;
    MovementSettings = nullptr;
    NavMoverComponent.Reset();
    NavDataInterface.Reset();
}

void ULedgeSafeWalkingMode::Activate(const FMoverEventContext& Context, FName PrevModeName,
    const FMoverSimContext& SimContext, const FMoverTickStartData& StartState,
    FMoverSyncState* OutSyncState, FMoverAuxStateContext* OutAuxState)
{
    Super::Activate(Context, PrevModeName, SimContext, StartState, OutSyncState, OutAuxState);
    NavDataInterface = GetNavData();
    if (SmoothMode)
    {
        SmoothMode->Activate(Context, PrevModeName, SimContext, StartState, OutSyncState, OutAuxState);
    }
}

void ULedgeSafeWalkingMode::Deactivate(const FMoverEventContext& Context, FName NextModeName,
    const FMoverSimContext& SimContext)
{
    if (SmoothMode)
    {
        SmoothMode->Deactivate(Context, NextModeName, SimContext);
    }
    Super::Deactivate(Context, NextModeName, SimContext);
}

bool ULedgeSafeWalkingMode::ApplySettings() const
{
    if (!MovementSettings || !NativeSettings || !SmoothMode)
    {
        return false;
    }
    const auto& Settings = *MovementSettings;
    NativeSettings->bShouldRemainVertical = Settings.bShouldRemainVertical;
    NativeSettings->MaxSpeed = FMath::Max(0.0f, Settings.MaxSpeed);
    NativeSettings->MaxWalkSlopeCosine = Settings.MaxWalkSlopeCosine;
    NativeSettings->MaxStepHeight = Settings.MaxStepHeight;
    NativeSettings->FloorSweepDistance = Settings.FloorSweepDistance;
    NativeSettings->bUseFlatBaseForFloorChecks = Settings.bUseFlatBaseForFloorChecks;
    NativeSettings->PerchRadiusThreshold = Settings.PerchRadiusThreshold;
    NativeSettings->bIgnoreBaseRotation = Settings.bIgnoreBaseRotation;

    // Translate the contract into native parameters; algorithm-specific tuning stays at the implementation's defaults.
    const auto* Acceleration = FindFProperty<FFloatProperty>(SmoothMode->GetClass(), TEXT("Acceleration"));
    const auto* Deceleration = FindFProperty<FFloatProperty>(SmoothMode->GetClass(), TEXT("Deceleration"));
    const auto* FloorPolicy = FindFProperty<FEnumProperty>(SmoothMode->GetClass(), TEXT("FloorCheckPolicy"));
    if (!ensure(Acceleration && Deceleration && FloorPolicy))
    {
        return false;
    }
    Acceleration->SetPropertyValue_InContainer(SmoothMode.Get(), Settings.Acceleration);
    Deceleration->SetPropertyValue_InContainer(SmoothMode.Get(), Settings.Deceleration);
    FloorPolicy->GetUnderlyingProperty()->SetIntPropertyValue(
        FloorPolicy->ContainerPtrToValuePtr<void>(SmoothMode.Get()), static_cast<uint64>(Settings.FloorCheckPolicy));
    return true;
}

const INavigationDataInterface* ULedgeSafeWalkingMode::GetNavData() const
{
    const auto* World = GetWorld();
    const auto* NavMover = NavMoverComponent.Get();
    const auto* NavSystem = World ? Cast<UNavigationSystemV1>(World->GetNavigationSystem()) : nullptr;
    return NavSystem && NavMover
        ? NavSystem->GetNavDataForProps(NavMover->GetNavAgentPropertiesRef(), NavMover->GetNavLocation())
        : nullptr;
}

bool ULedgeSafeWalkingMode::FindNavFloor(const FVector& TestLocation, FNavLocation& OutNavFloorLocation,
                                       const INavigationDataInterface* NavData) const
{
    const auto* NavMover = NavMoverComponent.Get();
    if (!NavData || !NavMover)
    {
        return false;
    }

    // Use NavWalking's agent-sized search extent and the native navigation projection query.
    const auto& AgentProps = NavMover->GetNavAgentPropertiesRef();
    const auto SearchRadius = AgentProps.AgentRadius * 2.0f;
    const auto SearchHeight = AgentProps.AgentHeight * AgentProps.NavWalkingSearchHeightScale;
    return NavData->ProjectPoint(TestLocation, OutNavFloorLocation, FVector(SearchRadius, SearchRadius, SearchHeight));
}

void ULedgeSafeWalkingMode::GenerateMove_Implementation(const FMoverSimContext& SimContext,
    const FMoverTickStartData& StartState, const FMoverTimeStep& TimeStep, FProposedMove& OutProposedMove) const
{
    OutProposedMove = FProposedMove();
    if (ApplySettings())
    {
        SmoothMode->GenerateMove(SimContext, StartState, TimeStep, OutProposedMove);
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
        !MovementSettings || !SmoothMode ||
        !StartingSyncState || DeltaSeconds <= UE_SMALL_NUMBER)
    {
        return;
    }

    if (!ApplySettings())
    {
        return;
    }

    if (!NavDataInterface.IsValid())
    {
        NavDataInterface = GetNavData();
    }

    const auto* NavData = NavDataInterface.Get();
    const auto* NavMover = NavMoverComponent.Get();
    const auto UpDirection = Mover->GetUpDirection();
    const auto StartingLocation = UpdatedComponent->GetComponentLocation();
    const auto StartingFeetLocation = NavMover ? NavMover->GetFeetLocation() : StartingLocation;
    FNavLocation StartingNavLocation;
    // Projection can find a nearby polygon even when the pawn is outside it; never snap onto that polygon.
    const auto bHasNavFloor = NavMover && NavData &&
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
        if (!MovementSettings->bSlideAlongNavMeshEdge && !AllowedPlanarDelta.Equals(DesiredPlanarDelta, NavLocationTolerance))
        {
            WalkingParams.ProposedMove.LinearVelocity = FVector::ZeroVector;
        }
        else if (!DesiredPlanarDelta.IsNearlyZero())
        {
            // Restrict horizontal travel only; native SmoothWalking handles height, slopes and steps.
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
        // Idle simulation rebuilds these at the reverted position instead of using the rejected destination.
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
        SmoothMode->SimulationTick(WalkingParams, WalkingOutput);
        FNavLocation FinalNavLocation;
        const auto FinalFeetLocation = NavMover ? NavMover->GetFeetLocation()
                                               : UpdatedComponent->GetComponentLocation();
        const auto bWithinBoundary = bHasNavFloor
            ? NavData->FindMoveAlongSurface(StartingNavLocation, FinalFeetLocation, FinalNavLocation) &&
              NavData->IsNodeRefValid(FinalNavLocation.NodeRef) &&
              FVector::VectorPlaneProject(FinalNavLocation.Location - FinalFeetLocation, UpDirection)
                  .IsNearlyZero(NavLocationTolerance)
            : FVector::VectorPlaneProject(UpdatedComponent->GetComponentLocation() - StartingLocation, UpDirection)
                  .IsNearlyZero(UE_KINDA_SMALL_NUMBER);
        // Native walking requests an air mode when floor support is lost; reject that trial in this ground-only mode.
        if (bWithinBoundary && WalkingOutput.MovementEndState.NextModeName.IsNone())
        {
            OutputState = MoveTemp(WalkingOutput);
            return;
        }

        ScopedMove.RevertMove();
        DiscardTrialCaches();
        WalkingParams.ProposedMove.LinearVelocity = FVector::ZeroVector;
    }

    // If idle movement also crossed the boundary, synchronize its output with the reverted pose.
    OutputState = MoveTemp(WalkingOutput);
    auto& OutputSyncState = OutputState.SyncState.SyncStateCollection.FindOrAddMutableDataByType<FMoverDefaultSyncState>();
    OutputSyncState = *StartingSyncState;
    OutputSyncState.MoveDirectionIntent = FVector::ZeroVector;
    OutputSyncState.SetTransforms_WorldSpace(UpdatedComponent->GetComponentLocation(), UpdatedComponent->GetComponentRotation(),
        FVector::ZeroVector, FVector::ZeroVector, StartingSyncState->GetMovementBase(),
        StartingSyncState->GetMovementBaseBoneName());
    UpdatedComponent->ComponentVelocity = FVector::ZeroVector;
    OutputState.MovementEndState.ResetToDefaults();
}
