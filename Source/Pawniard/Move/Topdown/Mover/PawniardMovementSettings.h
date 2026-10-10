// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DefaultMovementSet/Modes/WalkingMode.h"
#include "MovementMode.h"
#include "PawniardMovementSettings.generated.h"

/**
 * Implementation-independent movement contract for Pawniard. Extend this type when adding movement behavior.
 * Movement modes declare this type in SharedSettingsClasses and consume Mover's shared instance.
 */
UCLASS(BlueprintType, Blueprintable, EditInlineNew, DisplayName="Movement Settings (Pawniard)")
class PAWNIARD_API UPawniardMovementSettings : public UObject, public IMovementSettingsInterface
{
    GENERATED_BODY()

public:
    virtual FString GetDisplayName() const override;

    /** Keep the actor upright relative to the Mover's up direction. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="General")
    bool bShouldRemainVertical = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Movement", meta=(ClampMin="0", ForceUnits="cm/s"))
    float MaxSpeed = 800.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Movement", meta=(ClampMin="0", ForceUnits="cm/s^2"))
    float Acceleration = 4000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Movement", meta=(ClampMin="0", ForceUnits="cm/s^2"))
    float Deceleration = 4000.0f;

    /** Cosine of the maximum walkable slope angle. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ground Movement", meta=(ClampMin="0", ClampMax="1"))
    float MaxWalkSlopeCosine = 0.71f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ground Movement", meta=(ClampMin="0", ForceUnits="cm"))
    float MaxStepHeight = 40.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ground Movement", meta=(ClampMin="0", ForceUnits="cm"))
    float FloorSweepDistance = 40.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ground Movement")
    bool bUseFlatBaseForFloorChecks = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ground Movement", meta=(ClampMin="0", ForceUnits="cm"))
    float PerchRadiusThreshold = 0.0f;

    /** Keep world orientation when the movement base rotates; position still follows the base. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ground Movement")
    bool bIgnoreBaseRotation = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ground Movement")
    EStaticFloorCheckPolicy FloorCheckPolicy = EStaticFloorCheckPolicy::OnDynamicBaseOnly;

    /** Slide along the NavMesh boundary instead of stopping when the requested move crosses it. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="NavMesh Constraints")
    bool bSlideAlongNavMeshEdge = true;
};
