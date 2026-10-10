// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MovementMode.h"
#include "UObject/WeakInterfacePtr.h"
#include "LedgeSafeWalkingMode.generated.h"

class INavigationDataInterface;
class UCommonLegacyMovementSettings;
class UNavMoverComponent;
class UPawniardMovementSettings;
struct FNavLocation;

/** Native SmoothWalking driven by Pawniard's shared settings and constrained to the NavMesh. */
UCLASS(Blueprintable, DisplayName="LedgeSafeWalkingMode (Pawniard)")
class PAWNIARD_API ULedgeSafeWalkingMode : public UBaseMovementMode
{
    GENERATED_BODY()

public:
    ULedgeSafeWalkingMode();
    virtual void Activate(const FMoverEventContext& Context, FName PrevModeName,
                          const FMoverSimContext& SimContext, const FMoverTickStartData& StartState,
                          FMoverSyncState* OutSyncState, FMoverAuxStateContext* OutAuxState) override;
    virtual void Deactivate(const FMoverEventContext& Context, FName NextModeName,
                            const FMoverSimContext& SimContext) override;

    virtual void OnRegistered(FName ModeName, const FMoverSimContext& SimContext) override;
    virtual void OnUnregistered(const FMoverSimContext& SimContext) override;

    virtual void GenerateMove_Implementation(const FMoverSimContext& SimContext,
                                             const FMoverTickStartData& StartState,
                                             const FMoverTimeStep& TimeStep,
                                             FProposedMove& OutProposedMove) const override;
    virtual void SimulationTick_Implementation(const FSimulationTickParams& Params,
                                               FMoverTickEndData& OutputState) override;

private:
    void UnregisterNativeMode(const FMoverSimContext& SimContext);
    bool ApplySettings() const;
    const INavigationDataInterface* GetNavData() const;
    bool FindNavFloor(const FVector& TestLocation, FNavLocation& OutNavFloorLocation,
                      const INavigationDataInterface* NavData) const;

    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<UPawniardMovementSettings> MovementSettings;

    // A transient view shared by native consumers at runtime; Pawniard settings remain the authored contract.
    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<UCommonLegacyMovementSettings> NativeSettings;

    // SmoothWalking's constructor is not exported; compose a reflected instance through its public base API.
    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<UBaseMovementMode> SmoothMode;

    TWeakObjectPtr<UNavMoverComponent> NavMoverComponent;
    TWeakInterfacePtr<const INavigationDataInterface> NavDataInterface;
};
