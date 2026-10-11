// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/TrajectoryTypes.h"
#include "HAL/CriticalSection.h"
#include "PoseSearch/PoseSearchFeatureChannel_Curve.h"
#include "PawniardPoseSearchSlopeChannel.generated.h"

class UAnimInstance;

namespace UE::PoseSearch
{
    struct IPoseHistory;
}

/**
 * Matches an authored slope curve against movement of a native trajectory sample across updates.
 * Positive degrees mean ascending and negative degrees mean descending. This measures
 * movement, not ground contact, so steps and airborne trajectories can also contribute.
 */
UCLASS(BlueprintType, NotBlueprintable, EditInlineNew, CollapseCategories,
    meta = (DisplayName = "Trajectory Slope (Pawniard)"))
class PAWNIARD_API UPawniardPoseSearchSlopeChannel : public UPoseSearchFeatureChannel_Curve
{
    GENERATED_BODY()

public:
    UPawniardPoseSearchSlopeChannel();

    /** Compare snapshots from this many seconds of continuous animation updates. SampleTimeOffset selects the native trajectory point to track. */
    UPROPERTY(EditAnywhere, Category = "Trajectory", meta = (ClampMin = "0.001", Units = "s"))
    float SamplingWindow = 0.1f;

    /** Reset the snapshot history after a discontinuous position change. Zero disables the distance check. */
    UPROPERTY(EditAnywhere, Category = "Trajectory", meta = (ClampMin = "0", Units = "cm"))
    float HistoryResetDistance = 1000.0f;

    /** World-space up axis used to separate horizontal travel from height change. */
    UPROPERTY(EditAnywhere, Category = "Trajectory")
    FVector UpDirectionWS = FVector::UpVector;

    /** Below this horizontal speed, query a neutral slope instead of a running slope. */
    UPROPERTY(EditAnywhere, Category = "Trajectory", meta = (ClampMin = "0", ForceUnits = "cm/s"))
    float MinHorizontalSpeed = 300.0f;

    /** Fade from neutral to the measured slope between MinHorizontalSpeed and this speed. */
    UPROPERTY(EditAnywhere, Category = "Trajectory", meta = (ClampMin = "0", ForceUnits = "cm/s"))
    float FullWeightHorizontalSpeed = 600.0f;

    /** Limit the query to forward travel when the authored slope clips are forward locomotion. */
    UPROPERTY(EditAnywhere, Category = "Trajectory")
    bool bForwardOnly = true;

    UPROPERTY(EditAnywhere, Category = "Trajectory",
        meta = (EditCondition = "bForwardOnly", ClampMin = "-1", ClampMax = "1"))
    float MinForwardDot = 0.5f;

    UPROPERTY(EditAnywhere, Category = "Trajectory",
        meta = (EditCondition = "bForwardOnly", ClampMin = "-1", ClampMax = "1"))
    float FullWeightForwardDot = 0.85f;

    virtual void BuildQuery(UE::PoseSearch::FSearchContext& SearchContext) const override;

#if WITH_EDITOR
    virtual bool CanEditChange(const FProperty* InProperty) const override;
#endif

private:
    struct FInstanceHistory
    {
        FTransformTrajectory Snapshots;
        const UE::PoseSearch::IPoseHistory* Source = nullptr;
        uint64 LastFrame = MAX_uint64;
        float AvailableSeconds = 0.0f;
        float SamplingWindow = 0.0f;
        float SampleTimeOffset = 0.0f;
    };

    float SampleInstanceSlope(const UAnimInstance& AnimInstance, const UE::PoseSearch::IPoseHistory& PoseHistory) const;
    float CalculateSlope(const FTransformTrajectory& Trajectory, float EndTime) const;

    // Channels are shared assets; runtime histories must neither mix characters nor keep them alive.
    mutable FCriticalSection HistoriesMutex;
    mutable TMap<TWeakObjectPtr<const UAnimInstance>, FInstanceHistory> InstanceHistories;
    mutable uint64 LastCleanupFrame = 0;
};
