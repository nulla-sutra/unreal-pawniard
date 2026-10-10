// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DefaultMovementSet/Modes/NavWalkingMode.h"
#include "LedgeSafeWalkingMode.generated.h"

class UWalkingMode;

/** Native SmoothWalking simulation constrained to the NavMesh for both manual and navigation inputs. */
UCLASS(Blueprintable, DisplayName="LedgeSafeWalkingMode (Pawniard)", HideCategories=("NavMesh Movement"))
class PAWNIARD_API ULedgeSafeWalkingMode : public UNavWalkingMode
{
    GENERATED_BODY()

public:
    ULedgeSafeWalkingMode();
    virtual void Activate(const FMoverEventContext& Context, FName PrevModeName,
                          const FMoverSimContext& SimContext, const FMoverTickStartData& StartState,
                          FMoverSyncState* OutSyncState, FMoverAuxStateContext* OutAuxState) override;
    virtual void Deactivate(const FMoverEventContext& Context, FName NextModeName,
                            const FMoverSimContext& SimContext) override;

protected:
    virtual void OnRegistered(FName ModeName, const FMoverSimContext& SimContext) override;
    virtual void OnUnregistered(const FMoverSimContext& SimContext) override;

public:
    virtual void GenerateMove_Implementation(const FMoverSimContext& SimContext,
                                             const FMoverTickStartData& StartState,
                                             const FMoverTimeStep& TimeStep,
                                             FProposedMove& OutProposedMove) const override;
    virtual void SimulationTick_Implementation(const FSimulationTickParams& Params,
                                               FMoverTickEndData& OutputState) override;

    /** Editable template; its Mover-owned runtime copy handles both move generation and walking simulation. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Instanced, Category="Mover|Smooth Walking",
        meta=(DisplayName="Smooth Walking Settings"))
    TObjectPtr<UWalkingMode> MoveGenerationSettings;

private:
    // The base exposes public lifecycle methods; UWalkingMode makes its overrides protected.
    UPROPERTY(Transient)
    TObjectPtr<UBaseMovementMode> ActiveWalkingMode;
};
