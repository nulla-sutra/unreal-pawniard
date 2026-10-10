// Copyright Epic Games, Inc. All Rights Reserved.
// Adapted for Pawniard: movement-independent environment sampling.

#pragma once

#include "BoneControllers/AnimNode_FootPlacement.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "AnimNode_PawniardFootPlacement.generated.h"

class UPrimitiveComponent;
class UWorld;

namespace UE::Anim::PawniardFootPlacement
{
    struct FEvaluationContext;

    // Platform transforms are cached on the game thread; collision samples are taken during evaluation.
    struct FGroundSample
    {
        bool bHit = false;
        bool bSameSupport = false;
        bool bHasSupportTransform = false;
        FVector QueryPointWS = FVector::ZeroVector;
        FVector PositionWS = FVector::ZeroVector;
        FVector NormalWS = FVector::UpVector;
        TWeakObjectPtr<UPrimitiveComponent> Support;
        FTransform SupportTransform = FTransform::Identity;
        FTransform PreviousSupportTransform = FTransform::Identity;
    };

    struct FSceneSnapshot
    {
        bool bCanEvaluate = false;
        TWeakObjectPtr<UWorld> World;
        FCollisionQueryParams QueryParams;
        bool bHasBodyCollision = false;
        FCollisionShape BodyCollisionShape;
        FTransform BodyTransformWS = FTransform::Identity;
        ECollisionChannel BodyCollisionChannel = ECC_Pawn;
        FCollisionResponseParams BodyCollisionResponses;
        float TeleportDistanceThreshold = 0.0f;
        TArray<FGroundSample> Supports;
    };
}

USTRUCT(BlueprintInternalUseOnly, Experimental)
struct PAWNIARD_API FAnimNode_PawniardFootPlacement : public FAnimNode_SkeletalControlBase
{
    GENERATED_BODY()

public:
    // Graph measures actual foot motion; Manual uses a speed curve when available.
    UPROPERTY(EditAnywhere, Category = "Settings")
    EWarpingEvaluationMode PlantSpeedMode = EWarpingEvaluationMode::Graph;

    UPROPERTY(EditAnywhere, Category = "Settings")
    FBoneReference IKFootRootBone;

    UPROPERTY(EditAnywhere, Category = "Settings")
    FBoneReference PelvisBone;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (PinHiddenByDefault))
    FFootPlacementPelvisSettings PelvisSettings;

    UPROPERTY(EditAnywhere, Category = "Settings")
    TArray<FFootPlacemenLegDefinition> LegDefinitions;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (PinHiddenByDefault))
    FFootPlacementPlantSettings PlantSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (PinHiddenByDefault))
    FFootPlacementInterpolationSettings InterpolationSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (PinHiddenByDefault))
    FFootPlacementTraceSettings TraceSettings;

    UPROPERTY(EditAnywhere, Category = Settings, meta = (PinHiddenByDefault))
    FVector BaseTranslationDelta = FVector::ZeroVector;

public:
    // FK targets work even when the animations do not contain baked IK bone tracks.
    UPROPERTY(EditAnywhere, Category = "Settings")
    bool bUseInputIKTargets = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Environment", meta = (PinHiddenByDefault))
    FVector UpDirectionWS = FVector::UpVector;

    UPROPERTY(EditAnywhere, Category = "Environment", meta = (ClampMin = "0", ClampMax = "89"))
    float MaxGroundSlopeAngle = 45.0f;

    UPROPERTY(EditAnywhere, Category = "Environment", meta = (ClampMin = "0", Units = "cm"))
    float AutoGroundDistance = 15.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides", meta = (PinHiddenByDefault))
    bool bOverrideGrounded = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides",
        meta = (PinHiddenByDefault, EditCondition = "bOverrideGrounded"))
    bool bGrounded = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides", meta = (PinHiddenByDefault))
    bool bOverrideVelocity = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides",
        meta = (PinHiddenByDefault, EditCondition = "bOverrideVelocity"))
    FVector VelocityWS = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides", meta = (PinHiddenByDefault))
    bool bOverrideGroundPlane = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides",
        meta = (PinHiddenByDefault, EditCondition = "bOverrideGroundPlane"))
    FVector GroundLocationWS = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides",
        meta = (PinHiddenByDefault, EditCondition = "bOverrideGroundPlane"))
    FVector GroundNormalWS = FVector::UpVector;

    // A rising edge resets temporal state without requiring the caller to pulse for one frame.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "State Overrides", meta = (PinHiddenByDefault))
    bool bReset = false;

    FAnimNode_PawniardFootPlacement();

    // FAnimNode_Base interface
    virtual bool HasPreUpdate() const override { return true; }
    virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;
    virtual void GatherDebugData(FNodeDebugData& DebugData) override;
    // End of FAnimNode_Base interface

    // FAnimNode_SkeletalControlBase interface
    virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;

protected:
    virtual void UpdateInternal(const FAnimationUpdateContext& Context) override;
    virtual void EvaluateSkeletalControl_AnyThread(
        FComponentSpacePoseContext& Output,
        TArray<FBoneTransform>& OutBoneTransforms) override;
    virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
    // End of FAnimNode_SkeletalControlBase

    // FAnimNode_SkeletalControlBase interface
    virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
    // End of FAnimNode_SkeletalControlBase interface
private:
    // Gather raw or trivially calculated values from input pose
    void GatherPelvisDataFromInputs(const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context);
    void GatherLegDataFromInputs(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        UE::Anim::FootPlacement::FLegRuntimeData& LegData,
        const FFootPlacemenLegDefinition& LegDef);

    void CalculateFootMidpoint(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        TConstArrayView<UE::Anim::FootPlacement::FLegRuntimeData> LegData,
        FVector& OutMidpoint) const;

    // Calculate procedural adjustments before solving the desired pelvis position
    void ProcessComponentState(const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context);
    void ProcessFootAlignment(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        UE::Anim::FootPlacement::FLegRuntimeData& LegData);

    // Calculate the desired pelvis offset, based on procedural character/foot adjustments
    FTransform SolvePelvis(const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context);

    FTransform UpdatePelvisInterpolationRootSpace(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        const FTransform& TargetPelvisTransform);

    // Post-processing adjustments + fix hyper-extension/compression
    UE::Anim::FootPlacement::FPlantResult FinalizeFootAlignment(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        UE::Anim::FootPlacement::FLegRuntimeData& LegData,
        const FFootPlacemenLegDefinition& LegDef,
        const FTransform& PelvisTransformCS);

    const FTransform& GetRootToComponent() const;

private:
    UE::Anim::PawniardFootPlacement::FSceneSnapshot SceneSnapshot;
    TArray<UE::Anim::PawniardFootPlacement::FGroundSample> GroundSamples;
    bool bResetWasRequested = false;
    bool bBonesValid = false;
    FVector PreviousUpWS = FVector::UpVector;
    float CachedDeltaTime = 0.0f;

    TArray<UE::Anim::FootPlacement::FLegRuntimeData> LegsData;
    UE::Anim::FootPlacement::FPlantRuntimeSettings PlantRuntimeSettings;
    UE::Anim::FootPlacement::FPelvisRuntimeData PelvisData;
    UE::Anim::FootPlacement::FCharacterData CharacterData;

    // Whether we want to plant, independently from any dynamic pose adjustments we may do
    bool WantsToPlant(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        int32 LegIndex,
        const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const;

    // Get Alignment Alpha based on current foot speed
    // 0.0 is fully unaligned and the foot is in flight.
    // 1.0 is fully aligned and the foot is planted.
    float GetAlignmentAlpha(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const;

    // This function looks at both the foot bone and the ball bone, returning the smallest distance to the
    // planting plane. Note this distance can be negative, meaning it's penetrating.
    float CalcTargetPlantPlaneDistance(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const;

    struct FPelvisOffsetRangeForLimb
    {
        float MaxExtension;
        float MinExtension;
        float DesiredExtension;
    };

    // Find the horizontal pelvis offset range for the foot to reach:
    void FindPelvisOffsetRangeForLimb(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        const UE::Anim::FootPlacement::FLegRuntimeData& LegData,
        const FVector& PlantTargetLocationCS,
        const FTransform& PelvisTransformCS,
        FPelvisOffsetRangeForLimb& OutPelvisOffsetRangeCS) const;

    // Adjust LastPlantTransformWS to current, to have the foot pivot around the ball instead of the ankle
    FTransform GetFootPivotAroundBallWS(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose,
        const FTransform& LastPlantTransformWS) const;

    // Align the transform the provided world space ground plant plane.
    // Also outputs the twist along the ground plane needed to get there
    void AlignPlantToGround(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        const FPlane& PlantPlaneWS,
        const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose,
        FTransform& InOutFootTransformWS,
        FQuat& OutTwistCorrection) const;

    // Handles horizontal interpolation when unlocking the plant
    FTransform UpdatePlantOffsetInterpolation(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        UE::Anim::FootPlacement::FLegRuntimeData::FInterpolationData& InOutInterpData) const;

    // Handles the interpolation of the planting plane. Because the plant transform is specified with respect to the
    // planting plane, it cannot change abruptly without causing an animation pop. It must be interpolated instead.
    void UpdatePlantingPlaneInterpolation(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        int32 LegIndex,
        const FTransform& FootTransformWS,
        const FTransform& LastAlignedFootTransform,
        const float AlignmentAlpha,
        FPlane& InOutPlantPlane,
        const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose,
        UE::Anim::FootPlacement::FLegRuntimeData::FInterpolationData& InOutInterpData);

    // Checks unplanting and replanting conditions to determine if the foot is planted
    void DeterminePlantType(
        const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
        int32 LegIndex,
        const FTransform& FKTransformWS,
        const FTransform& CurrentBoneTransformWS,
        UE::Anim::FootPlacement::FLegRuntimeData::FPlantData& InOutPlantData,
        const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const;

    float GetMaxLimbExtension(const float DesiredExtension, const float LimbLength) const;
    float GetMinLimbExtension(const float DesiredExtension, const float LimbLength) const;

    void ResetRuntimeData();

    bool bIsFirstUpdate = false;
    FGraphTraversalCounter UpdateCounter;
};
