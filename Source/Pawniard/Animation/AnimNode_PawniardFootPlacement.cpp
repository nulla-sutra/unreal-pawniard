// Copyright Epic Games, Inc. All Rights Reserved.

#include "AnimNode_PawniardFootPlacement.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimTrace.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ShapeComponent.h"
#include "GameFramework/Actor.h"
#include "AnimationRuntime.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AnimNode_PawniardFootPlacement)

DECLARE_CYCLE_STAT(TEXT("Foot Placement Eval"), STAT_PawniardFootPlacement_Eval, STATGROUP_Anim);

namespace UE::Anim::PawniardFootPlacement
{
    struct FEvaluationContext
    {
        FEvaluationContext(FComponentSpacePoseContext& InPose, const FSceneSnapshot& InScene,
            const FVector& UpWS, float MaxSlopeAngle, float DeltaTime)
            : CSPContext(InPose), Scene(InScene), OwningComponentToWorld(InPose.AnimInstanceProxy->GetComponentTransform()),
              UpdateDeltaTime(DeltaTime), ApproachDirWS(UpWS.ContainsNaN() ? -FVector::UpVector : -UpWS.GetSafeNormal())
        {
            if (ApproachDirWS.IsNearlyZero())
            {
                ApproachDirWS = -FVector::UpVector;
            }
            ApproachDirCS = OwningComponentToWorld.InverseTransformVector(ApproachDirWS).GetSafeNormal();
            WorldUnitsPerComponentUnit = OwningComponentToWorld.TransformVector(ApproachDirCS).Size();
            GroundNormal = -ApproachDirWS;
            MinGroundNormalDot = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(MaxSlopeAngle, 0.0f, 89.0f)));
        }

        FComponentSpacePoseContext& CSPContext;
        const FSceneSnapshot& Scene;
        FTransform OwningComponentToWorld;
        float UpdateDeltaTime;
        float WorldUnitsPerComponentUnit = 1.0f;
        FVector ApproachDirWS;
        FVector ApproachDirCS;
        FVector GroundLocation = FVector::ZeroVector;
        float MinGroundNormalDot = 0.0f;
        FVector GroundNormal;
        bool bGrounded = false;
        bool bGroundPlaneOverride = false;
    };

    static FGroundSample FindPlantTraceImpact(const FEvaluationContext& Context,
        const FFootPlacementTraceSettings& Settings, const FVector& PointWS,
        bool bComplex, bool bUseSphereTrace, const FGroundSample* SupportSnapshot)
    {
        FGroundSample Sample;
        Sample.QueryPointWS = PointWS;
        const auto* World = Context.Scene.World.Get();
        if (!World || !Settings.bEnabled)
        {
            return Sample;
        }

        auto QueryParams = Context.Scene.QueryParams;
        QueryParams.bTraceComplex = bComplex;
        const auto Channel = UEngineTypes::ConvertToCollisionChannel(
            bComplex ? Settings.ComplexTraceChannel : Settings.SimpleTraceChannel);
        const auto Scale = Context.WorldUnitsPerComponentUnit;
        const auto StartWS = PointWS + Context.ApproachDirWS * Settings.StartOffset * Scale;
        const auto EndWS = PointWS + Context.ApproachDirWS * Settings.EndOffset * Scale;
        FHitResult Hit;
        auto bHit = false;
        // Match native timing: query the current evaluated target synchronously.
        if (bUseSphereTrace)
        {
            const auto Shape = FCollisionShape::MakeSphere(FMath::Max(0.01f, Settings.SweepRadius * Scale));
            bHit = World->SweepSingleByChannel(Hit, StartWS, EndWS, FQuat::Identity, Channel, Shape, QueryParams);
        }
        else
        {
            bHit = World->LineTraceSingleByChannel(Hit, StartWS, EndWS, Channel, QueryParams);
        }
        Sample.bHit = bHit && Hit.bBlockingHit && !Hit.bStartPenetrating
            && FVector::DotProduct(Hit.ImpactNormal.GetSafeNormal(), -Context.ApproachDirWS)
                >= Context.MinGroundNormalDot;
        if (Sample.bHit)
        {
            Sample.PositionWS = Hit.ImpactPoint;
            Sample.NormalWS = Hit.ImpactNormal.GetSafeNormal();
            Sample.Support = Hit.Component;
            // Never fetch component transforms from the animation worker. New supports are cached next PreUpdate.
            if (SupportSnapshot && SupportSnapshot->bHasSupportTransform && Sample.Support == SupportSnapshot->Support)
            {
                Sample.bHasSupportTransform = true;
                Sample.bSameSupport = SupportSnapshot->bSameSupport;
                Sample.SupportTransform = SupportSnapshot->SupportTransform;
                Sample.PreviousSupportTransform = SupportSnapshot->PreviousSupportTransform;
            }
        }
        return Sample;
    }

    static FGroundSample FindPlantPlane(const FEvaluationContext& Context,
        const FFootPlacementTraceSettings& Settings, const FVector& PointWS,
        bool bPreferSimple, const FGroundSample* SupportSnapshot = nullptr, bool bPreferPointContact = true)
    {
        const auto bComplexFirst = !bPreferSimple && !Settings.bDisableComplexTrace;
        const auto TracePreferredCollision = [&](bool bUseSphereTrace)
        {
            auto Sample = FindPlantTraceImpact(Context, Settings, PointWS,
                bComplexFirst, bUseSphereTrace, SupportSnapshot);
            if (!Sample.bHit && !Settings.bDisableComplexTrace)
            {
                Sample = FindPlantTraceImpact(Context, Settings, PointWS,
                    !bComplexFirst, bUseSphereTrace, SupportSnapshot);
            }
            return Sample;
        };
        if (bPreferPointContact)
        {
            // A nearby ledge must not mask valid ground directly beneath the foot.
            const auto Sample = TracePreferredCollision(false);
            if (Sample.bHit)
            {
                return Sample;
            }
        }
        // Keep sphere sweeps for gaps where neither point trace found valid ground.
        return TracePreferredCollision(true);
    }

    static FGroundSample FindBodySupport(const FEvaluationContext& Context, float GroundTolerance)
    {
        FGroundSample Sample;
        const auto& Scene = Context.Scene;
        const auto* World = Scene.World.Get();
        if (!World || !Scene.bHasBodyCollision)
        {
            return Sample;
        }

        auto QueryParams = Scene.QueryParams;
        QueryParams.bTraceComplex = false;
        const auto BodyLocationWS = Scene.BodyTransformWS.GetLocation();
        const auto BodyRotation = Scene.BodyTransformWS.GetRotation();
        const auto UpWS = -Context.ApproachDirWS;
        const auto& Shape = Scene.BodyCollisionShape;
        auto SupportExtentAlongUp = 0.0;
        auto CurvatureAllowance = 0.0;
        if (Shape.IsCapsule())
        {
            const auto Radius = Shape.GetCapsuleRadius();
            SupportExtentAlongUp = Radius + FMath::Max(0.0f, Shape.GetCapsuleHalfHeight() - Radius)
                * FMath::Abs(FVector::DotProduct(BodyRotation.GetAxisZ(), UpWS));
            CurvatureAllowance = Radius;
        }
        else if (Shape.IsSphere())
        {
            SupportExtentAlongUp = Shape.GetSphereRadius();
            CurvatureAllowance = SupportExtentAlongUp;
        }
        else if (Shape.IsBox())
        {
            const auto Extent = Shape.GetBox();
            SupportExtentAlongUp = FMath::Abs(FVector::DotProduct(BodyRotation.GetAxisX(), UpWS)) * Extent.X
                + FMath::Abs(FVector::DotProduct(BodyRotation.GetAxisY(), UpWS)) * Extent.Y
                + FMath::Abs(FVector::DotProduct(BodyRotation.GetAxisZ(), UpWS)) * Extent.Z;
        }
        else
        {
            return Sample;
        }

        // A step can raise the body before its rounded end is over the tread.
        constexpr auto ProbeLift = 2.0f;
        FHitResult Hit;
        const auto bHit = World->SweepSingleByChannel(Hit,
            BodyLocationWS - Context.ApproachDirWS * ProbeLift,
            BodyLocationWS + Context.ApproachDirWS * (GroundTolerance + CurvatureAllowance),
            BodyRotation, Scene.BodyCollisionChannel, Shape, QueryParams, Scene.BodyCollisionResponses);
        const auto ImpactNormalWS = Hit.ImpactNormal.GetSafeNormal();
        const auto NormalUpDot = FVector::DotProduct(ImpactNormalWS, UpWS);
        if (!bHit || !Hit.bBlockingHit || Hit.bStartPenetrating || !(NormalUpDot >= Context.MinGroundNormalDot))
        {
            return Sample;
        }

        const auto BodyBaseWS = BodyLocationWS - UpWS * SupportExtentAlongUp;
        const auto BasePlaneDistance = FVector::DotProduct(BodyBaseWS - Hit.ImpactPoint, ImpactNormalWS) / NormalUpDot;
        // Preserve close physical support on slopes; floor height also covers rounded-edge step transitions.
        Sample.bHit = Hit.Distance - ProbeLift <= GroundTolerance + KINDA_SMALL_NUMBER
            || FMath::Abs(BasePlaneDistance) <= GroundTolerance + KINDA_SMALL_NUMBER;
        if (Sample.bHit)
        {
            Sample.PositionWS = Hit.ImpactPoint;
            Sample.NormalWS = ImpactNormalWS;
        }
        return Sample;
    }

    // Since we are calculating in world-space, when too far from the origin FMath::LinePlaneIntersection can introduce numerical error
    // and consider the line's start/end to be at the same location.
    // Use point-direction instead to avoid this
    // See UE-162275
    static FVector PointDirectionPlaneIntersection(const FVector Point, const FVector Direction, const FPlane Plane)
    {
        // A nearly parallel plane has no stable intersection; preserve the source point.
        const auto Denominator = FVector::DotProduct(Direction, Plane.GetNormal());
        return FMath::IsNearlyZero(Denominator) ? Point
            : Point + Direction * ((Plane.W - FVector::DotProduct(Point, Plane.GetNormal())) / Denominator);
    };

    static TOptional<float> GetDistanceToPlaneAlongDirection(const FVector& Location, const FPlane& PlantPlane, const FVector& ApproachDir)
    {
        // If approach dir is perpendicular to plant plane, distance is undefined
        if (FMath::IsNearlyZero(ApproachDir | PlantPlane))
        {
            return TOptional<float>();
        }

        const auto IntersectionLoc = UE::Anim::PawniardFootPlacement::PointDirectionPlaneIntersection(
            Location,
            -ApproachDir,
            PlantPlane);

        const auto IntersectionToLocation = Location - IntersectionLoc;
        const float DistanceToPlantPlane = IntersectionToLocation | -ApproachDir;
        return DistanceToPlantPlane;
    }

};

void FAnimNode_PawniardFootPlacement::FindPelvisOffsetRangeForLimb(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    const UE::Anim::FootPlacement::FLegRuntimeData& LegData,
    const FVector& InPlantTargetLocationCS,
    const FTransform& PelvisTransformCS,
    FPelvisOffsetRangeForLimb& OutPelvisOffsetRangeCS) const
{
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose = LegData.InputPose;
    const UE::Anim::FootPlacement::FLegRuntimeData::FBoneData& Bones = LegData.Bones;
    const float LimbLength = Bones.LimbLength;
    FVector PlantTargetLocationCS = InPlantTargetLocationCS;

    const auto HipToPelvis =
        LegInputPose.HipTransformCS.GetRelativeTransform(PelvisData.InputPose.FKTransformCS);
    const auto HipTransformCS = HipToPelvis * PelvisTransformCS;
    const auto HipLocationCS = HipTransformCS.GetLocation();

    const auto DesiredExtensionDelta =
        LegInputPose.FootTransformCS.GetLocation() - LegInputPose.HipTransformCS.GetLocation();

    const float DesiredExtensionSqrd = DesiredExtensionDelta.SizeSquared();
    const float DesiredExtension = FMath::Sqrt(DesiredExtensionSqrd);
    const float MaxExtension = GetMaxLimbExtension(DesiredExtension, LimbLength);

    const auto FootPlane = FPlane(PlantTargetLocationCS, -Context.ApproachDirCS);
    const float HipHeight = FootPlane.PlaneDot(HipLocationCS);

    FVector DesiredPlantTargetLocationCS = PlantTargetLocationCS;
    FVector MaxPlantTargetLocationCS = PlantTargetLocationCS;
    // Don't do horizontal adjustments if the foot is above the hip
    if (HipHeight > 0.0f)
    {
        // Project the input pose foot and hip to the ground-aligned foot plane
        const auto FKFootProjected = FVector::PointPlaneProject(LegInputPose.FootTransformCS.GetLocation(), FootPlane);
        const auto HipProjected = FVector::PointPlaneProject(HipLocationCS, FootPlane);

        // Project both our FK and IK feet to the foot plane, and calculate distances to the projected hip.
        FVector TargetFootToHip = HipProjected - PlantTargetLocationCS;
        float TargetFootOffset = FVector::Dist(HipProjected, PlantTargetLocationCS);
        float InitialFootOffset = FVector::Dist(HipProjected, FKFootProjected);

        // Move our IK foot to our FK foot by about the foot's length horizontally.
        // This will make our heel lift before it drops the hips when locking.
        const auto ToFKFoot = (FKFootProjected - PlantTargetLocationCS);
        const auto ToFKFootDir = ToFKFoot.GetSafeNormal();
        const float ToFKDistance = ToFKFoot.Size();
        PlantTargetLocationCS = PlantTargetLocationCS + ToFKFootDir * FMath::Min(ToFKDistance, Bones.FootLength) * PelvisSettings.HeelLiftRatio;

        // Calculate new correction offset and direction
        TargetFootToHip = HipProjected - PlantTargetLocationCS;
        TargetFootOffset = FVector::Dist(HipProjected, PlantTargetLocationCS);

        auto FindPlantLocationAdjustedByOrtogonalLimit =
            [	TargetFootOffset, TargetFootToHip,
                HipHeight, InitialFootOffset	]
            (const float LegLength, const float MaxHipOffset, const FVector& FootLocation)
        {
            FVector AdjustedPlantTargetLocationCS = FootLocation;

            // The minimum height our hip can be at after horizontal adjustments
            const float MinHeight = HipHeight - MaxHipOffset;
            const float MinHeightSqrd = FMath::Square(MinHeight);

            // Find how far our foot would be from the projected hip, if the leg was at max extension.
            const float LegLengthSqrd = FMath::Square(LegLength);
            const float MaxFootOffset = FMath::Sqrt(FMath::Max(0.0f, LegLengthSqrd - MinHeightSqrd));

            // If the input pose is already further than this, respect the input pose
            const float MaxFootOffsetClamped = FMath::Max(InitialFootOffset, MaxFootOffset);

            if (TargetFootOffset > MaxFootOffsetClamped)
            {
                // Move the foot towards the projected hip
                AdjustedPlantTargetLocationCS += (TargetFootOffset - MaxFootOffsetClamped) * TargetFootToHip.GetSafeNormal();
            }

            return AdjustedPlantTargetLocationCS;
        };

        MaxPlantTargetLocationCS = FindPlantLocationAdjustedByOrtogonalLimit(MaxExtension, PelvisSettings.MaxOffsetHorizontal, PlantTargetLocationCS);
        DesiredPlantTargetLocationCS = FindPlantLocationAdjustedByOrtogonalLimit(DesiredExtension,  PelvisSettings.MaxOffsetHorizontal, PlantTargetLocationCS);
    }

    // Taken from http://runevision.com/thesis/rune_skovbo_johansen_thesis.pdf
    // Chapter 7.4.2
    //	Intersections are found of a vertical line going through the original hip
    //	position and two spheres with their centers at the new ankle position (PlantTargetLocationCS)
    //	Sphere 1 has a radius of the distance between the hip and ankle in the input pose (DesiredExtension)
    //	Sphere 2 has a radius corresponding to the length of the leg from hip to ankle (MaxExtension).
    FVector MaxOffsetLocation;
    FVector DesiredOffsetLocation;
    FMath::SphereDistToLine(MaxPlantTargetLocationCS, MaxExtension, HipLocationCS - Context.ApproachDirCS * TraceSettings.EndOffset, Context.ApproachDirCS, MaxOffsetLocation);
    FMath::SphereDistToLine(DesiredPlantTargetLocationCS, DesiredExtension, HipLocationCS - Context.ApproachDirCS * TraceSettings.EndOffset, Context.ApproachDirCS, DesiredOffsetLocation);

    const float MaxOffset = (MaxOffsetLocation - HipLocationCS) | -Context.ApproachDirCS;
    const float DesiredOffset = (DesiredOffsetLocation - HipLocationCS) | -Context.ApproachDirCS;
    OutPelvisOffsetRangeCS.MaxExtension = MaxOffset;
    OutPelvisOffsetRangeCS.DesiredExtension = DesiredOffset;

    // Calculate min offset considering only the height of the foot
    // Poses where the foot's height is close to the hip's height are bad.
    const float MinExtension = GetMinLimbExtension(DesiredExtension, LimbLength);
    const auto MinOffsetLocation = DesiredPlantTargetLocationCS + -Context.ApproachDirCS * MinExtension;

    const float MinOffset = (MinOffsetLocation - HipLocationCS) | -Context.ApproachDirCS;
    // Limit pelvis compression adjustment by the height of the foot. We can always bring the foot closer to the ground in post-adjustments
    OutPelvisOffsetRangeCS.MinExtension = MinOffset - LegInputPose.DistanceToPlant;
}

float FAnimNode_PawniardFootPlacement::CalcTargetPlantPlaneDistance(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const
{
    const auto IKBallBoneCS = LegInputPose.FootToBall * LegInputPose.FootTransformCS;

    const auto IKGroundPlaneCS =
        FPlane(	PelvisData.InputPose.IKRootTransformCS.GetLocation(),
                PelvisData.InputPose.IKRootTransformCS.TransformVectorNoScale(FVector::UpVector));

    const TOptional<float> FootBaseDistance =
        UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(LegInputPose.FootTransformCS.GetLocation(), IKGroundPlaneCS, Context.ApproachDirCS);
    const TOptional<float> BallBaseDistance =
        UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(IKBallBoneCS.GetLocation(), IKGroundPlaneCS, Context.ApproachDirCS);

    const float PlantPlaneDistance = FMath::Min(FootBaseDistance.Get(0.0f), BallBaseDistance.Get(0.0f));
    return PlantPlaneDistance;
}

void FAnimNode_PawniardFootPlacement::AlignPlantToGround(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    const FPlane& PlantPlaneWS,
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose,
    FTransform& InOutFootTransformWS,
    FQuat& OutTwistCorrection) const
{
    const auto InputPoseFootTransformWS = LegInputPose.FootTransformCS * Context.OwningComponentToWorld;

    // It is assumed the distance from the plane defined by ik foot root to the ik reference, along the trace
    // direction, must remain the same.
    const auto IKFootRootWS = PelvisData.InputPose.IKRootTransformCS * Context.OwningComponentToWorld;
    const auto IKFootRootPlaneWS = FPlane(IKFootRootWS.GetLocation(), IKFootRootWS.TransformVectorNoScale(FVector::UpVector));
    const TOptional<float> IKFootRootToFootRootTargetDistance =
        UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(InputPoseFootTransformWS.GetLocation(), IKFootRootPlaneWS, Context.ApproachDirWS);

    const auto CorrectedPlaneIntersectionWS = UE::Anim::PawniardFootPlacement::PointDirectionPlaneIntersection(
        InOutFootTransformWS.GetLocation(),
        Context.ApproachDirWS,
        PlantPlaneWS);

    const auto CorrectedLocationWS =
        CorrectedPlaneIntersectionWS - (Context.ApproachDirWS * IKFootRootToFootRootTargetDistance.Get(0.0f));

    // The relationship between the ik reference and the normal of the plane defined by the ik foot root must also be
    // respected
    const auto PlanePlaneDeltaRotation = FQuat::FindBetweenNormals(IKFootRootPlaneWS.GetNormal(), PlantPlaneWS.GetNormal());
    const auto InputPoseAlignedRotationWS = PlanePlaneDeltaRotation * InputPoseFootTransformWS.GetRotation();

    // Find the rotation that will take us from the Aligned Input Pose to the Unaligned IK Foot
    const auto UnalignedIKFootToUnalignedInputPoseRotationDelta =
        InputPoseAlignedRotationWS.Inverse() * InOutFootTransformWS.GetRotation();
    const auto IKReferenceNormalFootSpace = InputPoseAlignedRotationWS.UnrotateVector(PlantPlaneWS.GetNormal());

    // Calculate and apply the amount of twist around the IK Root plane.
    FQuat OutSwing;
    UnalignedIKFootToUnalignedInputPoseRotationDelta.ToSwingTwist(IKReferenceNormalFootSpace,
        OutSwing,
        OutTwistCorrection);
    const auto AlignedRotationWS = InputPoseAlignedRotationWS * OutTwistCorrection;

    // Find the rotation that will take us from aligned to unaligned foot
    const auto AlignedToUnalignedRotationDelta =
        AlignedRotationWS.Inverse() * InOutFootTransformWS.GetRotation();
    // The rotation is a delta so we won't need to re-orient this vector
    const auto FootToBallDir = LegInputPose.FootToBall.GetTranslation().GetSafeNormal();
    FQuat AnkleTwist;
    AlignedToUnalignedRotationDelta.ToSwingTwist(FootToBallDir,
        OutSwing,
        AnkleTwist);
    // Counter the aligned ankle twist by the user-defined amount
    const auto TwistCorrectedRotationWS = AlignedRotationWS *  FQuat::Slerp(FQuat::Identity, AnkleTwist, PlantSettings.AnkleTwistReduction);

    InOutFootTransformWS = FTransform(TwistCorrectedRotationWS, CorrectedLocationWS, InputPoseFootTransformWS.GetScale3D());
}

FTransform FAnimNode_PawniardFootPlacement::UpdatePlantOffsetInterpolation(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    UE::Anim::FootPlacement::FLegRuntimeData::FInterpolationData& InOutInterpData) const
{
    const auto IkBaseSpringTranslation = UKismetMathLibrary::VectorSpringInterp(
        InOutInterpData.UnalignedFootOffset.GetTranslation(), FVector::ZeroVector, InOutInterpData.PlantOffsetTranslationSpringState,
        InterpolationSettings.UnplantLinearStiffness,
        InterpolationSettings.UnplantLinearDamping,
        Context.UpdateDeltaTime, 1.0f, 0.0f);

    // Since the alignment is just a translation offset, there's no need to calculate a different offset.
    const auto IkBaseSpringRotation = UKismetMathLibrary::QuaternionSpringInterp(
        InOutInterpData.UnalignedFootOffset.GetRotation(), FQuat::Identity, InOutInterpData.PlantOffsetRotationSpringState,
        InterpolationSettings.UnplantAngularStiffness,
        InterpolationSettings.UnplantAngularDamping,
        Context.UpdateDeltaTime, 1.0f, 0.0f);

    const auto IkBaseSpringOffset = FTransform(IkBaseSpringRotation, IkBaseSpringTranslation);
    return IkBaseSpringOffset;
}

void FAnimNode_PawniardFootPlacement::UpdatePlantingPlaneInterpolation(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context, int32 LegIndex,
    const FTransform& FootTransformWS, const FTransform& LastAlignedFootTransform,
    const float AlignmentAlpha, FPlane& InOutPlantPlane,
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose,
    UE::Anim::FootPlacement::FLegRuntimeData::FInterpolationData& InOutInterpData)
{
    using namespace UE::Anim::PawniardFootPlacement;
    const auto LastPlane = InOutPlantPlane;
    auto& Sample = GroundSamples[LegIndex];
    if (!Sample.QueryPointWS.Equals(FootTransformWS.GetLocation(), KINDA_SMALL_NUMBER))
    {
        // Locking, heel pivoting and separation can move the target after the contact check.
        Sample = FindPlantPlane(Context, TraceSettings, FootTransformWS.GetLocation(),
            false, &Context.Scene.Supports[LegIndex]);
    }
    const auto bFoundGround = Context.bGrounded && (Sample.bHit || Context.bGroundPlaneOverride);
    const auto SourcePointWS = Context.OwningComponentToWorld.TransformPosition(PelvisData.InputPose.IKRootTransformCS.GetLocation());
    auto ImpactPointWS = SourcePointWS;
    auto ImpactNormalWS = -Context.ApproachDirWS;
    if (bFoundGround)
    {
        ImpactPointWS = Sample.bHit ? Sample.PositionWS : Context.GroundLocation;
        ImpactNormalWS = Sample.bHit ? Sample.NormalWS : Context.GroundNormal;
    }
    InOutPlantPlane = FPlane(ImpactPointWS, ImpactNormalWS);
    if (!InterpolationSettings.bEnableFloorInterpolation || bIsFirstUpdate)
    {
        return;
    }

    auto Intersection = PointDirectionPlaneIntersection(FootTransformWS.GetLocation(), Context.ApproachDirWS, InOutPlantPlane);
    const auto PreviousIntersection = PointDirectionPlaneIntersection(FootTransformWS.GetLocation(), Context.ApproachDirWS, LastPlane);
    const auto PreviousAlignedIntersection = PointDirectionPlaneIntersection(LastAlignedFootTransform.GetLocation(), Context.ApproachDirWS, LastPlane);
    const auto UpWS = -Context.ApproachDirWS;
    const auto CurrentHeight = FVector::DotProduct(Intersection, UpWS);
    const auto PreviousHeight = FVector::DotProduct(PreviousIntersection, UpWS);
    const auto PreviousAlignedHeight = FVector::DotProduct(PreviousAlignedIntersection, UpWS);
    auto AdjustedHeight = FMath::Abs(PreviousAlignedHeight - CurrentHeight) < FMath::Abs(PreviousHeight - CurrentHeight)
        ? PreviousAlignedHeight : PreviousHeight;
    if (Context.bGrounded)
    {
        const auto Difference = CurrentHeight - AdjustedHeight;
        const auto MovementHeight = FMath::Abs(FVector::DotProduct(CharacterData.ComponentMoveDeltaWS, UpWS));
        AdjustedHeight += FMath::Clamp(Difference, -MovementHeight, MovementHeight);
    }
    const auto SpringHeight = UKismetMathLibrary::FloatSpringInterp(AdjustedHeight, CurrentHeight,
        InOutInterpData.GroundHeightSpringState, InterpolationSettings.FloorLinearStiffness,
        InterpolationSettings.FloorLinearDamping, Context.UpdateDeltaTime, 1.0f, 0.0f);
    Intersection += UpWS * (SpringHeight - CurrentHeight);
    if (bFoundGround && TraceSettings.MaxGroundPenetration >= 0.0f)
    {
        const auto Distance = GetDistanceToPlaneAlongDirection(Intersection,
            FPlane(ImpactPointWS, ImpactNormalWS), Context.ApproachDirWS);
        const auto Penetration = -Distance.Get(0.0f) - TraceSettings.MaxGroundPenetration * Context.WorldUnitsPerComponentUnit;
        if (Penetration > 0.0f)
        {
            Intersection += UpWS * Penetration;
        }
    }
    const auto NormalDelta = FQuat::FindBetweenNormals(LastPlane.GetNormal(), ImpactNormalWS);
    const auto NormalSpring = UKismetMathLibrary::QuaternionSpringInterp(FQuat::Identity, NormalDelta,
        InOutInterpData.GroundRotationSpringState, InterpolationSettings.FloorAngularStiffness,
        InterpolationSettings.FloorAngularDamping, Context.UpdateDeltaTime, 1.0f, 0.0f);
    InOutPlantPlane = FPlane(Intersection, NormalSpring.RotateVector(LastPlane.GetNormal()).GetSafeNormal());
}

void FAnimNode_PawniardFootPlacement::DeterminePlantType(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    int32 LegIndex,
    const FTransform& FKTransformWS,
    const FTransform& CurrentBoneTransformWS,
    UE::Anim::FootPlacement::FLegRuntimeData::FPlantData& InOutPlantData,
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const
{
    using namespace UE::Anim::FootPlacement;

    const bool bWasPlanted = InOutPlantData.PlantType != EPlantType::Unplanted;
    const bool bWantedToPlant = InOutPlantData.bWantsToPlant;

    InOutPlantData.bWantsToPlant = WantsToPlant(Context, LegIndex, LegInputPose);
    InOutPlantData.PlantType = EPlantType::Unplanted;

    if (!InOutPlantData.bWantsToPlant)
    {
        return;
    }

    // Test for un-plant
    if (bWasPlanted)
    {
        const auto PlantTranslationWS =
            CurrentBoneTransformWS.GetLocation() - FKTransformWS.GetLocation();

        // Don't consider the limits to be exceeded if replant radius == unplant radius.
        const bool bPlantTranslationExceeded =
            PlantSettings.ReplantRadiusRatio < 1.0f &&
            FVector::VectorPlaneProject(PlantTranslationWS, Context.ApproachDirWS).SizeSquared() > PlantRuntimeSettings.UnplantRadiusSqrd;
        const bool bPlantRotationExceeded =
            PlantSettings.ReplantAngleRatio < 1.0f &&
            FMath::Abs(InOutPlantData.TwistCorrection.W) <
            PlantRuntimeSettings.CosHalfUnplantAngle;

        if (!bPlantTranslationExceeded && !bPlantRotationExceeded)
        {
            // Carry over result from last plant.
            InOutPlantData.PlantType = InOutPlantData.LastPlantType;
        }
    }
    else if (!bWantedToPlant)
    {
        // If FK wasn't planted last frame, and it is on this frame, we're planted
        InOutPlantData.PlantType = UE::Anim::FootPlacement::EPlantType::Planted;
    }
    else // Test for re-plant
    {
        const auto PlantLocationDelta =
            CurrentBoneTransformWS.GetLocation() - FKTransformWS.GetLocation();

        const float LocationDeltaSizeSqrd = FVector::VectorPlaneProject(PlantLocationDelta, Context.ApproachDirWS).SizeSquared();

        const bool bLocationWithinBounds =
            LocationDeltaSizeSqrd <= PlantRuntimeSettings.ReplantRadiusSqrd;
        const bool bTwistWithinBounds =
            FMath::Abs(InOutPlantData.TwistCorrection.W) >=
            PlantRuntimeSettings.CosHalfReplantAngle;

        if (bLocationWithinBounds && bTwistWithinBounds)
        {
            InOutPlantData.PlantType = UE::Anim::FootPlacement::EPlantType::Replanted;
        }
    }
}

float FAnimNode_PawniardFootPlacement::GetMaxLimbExtension(const float DesiredExtension, const float LimbLength) const
{
    if (DesiredExtension > LimbLength)
    {
        return DesiredExtension;
    }

    const float RemainingLength = LimbLength - DesiredExtension;
    return DesiredExtension + RemainingLength * PlantSettings.MaxExtensionRatio;
}

float FAnimNode_PawniardFootPlacement::GetMinLimbExtension(const float DesiredExtension, const float LimbLength) const
{
    return FMath::Min(DesiredExtension, LimbLength * PlantSettings.MinExtensionRatio);
}

void FAnimNode_PawniardFootPlacement::ResetRuntimeData()
{
    // Relevance and teleport resets must preserve bone indices; CacheBones need not run again.
    PelvisData.Interpolation = UE::Anim::FootPlacement::FPelvisRuntimeData::FInterpolationData();
    CharacterData = UE::Anim::FootPlacement::FCharacterData();
    if (LegsData.Num() != LegDefinitions.Num())
    {
        LegsData.SetNum(LegDefinitions.Num());
        bBonesValid = false;
    }
    for (auto Index = 0; Index < LegsData.Num(); ++Index)
    {
        const auto Bones = LegsData[Index].Bones;
        LegsData[Index] = UE::Anim::FootPlacement::FLegRuntimeData();
        auto& Leg = LegsData[Index];
        Leg.Bones = Bones;
        Leg.Idx = Index;
        Leg.SpeedCurveName = LegDefinitions[Index].SpeedCurveName;
        Leg.DisableLockCurveName = LegDefinitions[Index].DisableLockCurveName;
        Leg.DisableLegCurveName = LegDefinitions[Index].DisableLegCurveName;
    }
    GroundSamples.Reset();
    bIsFirstUpdate = true;
}

bool FAnimNode_PawniardFootPlacement::WantsToPlant(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    int32 LegIndex,
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const
{

    if (!Context.bGrounded || (!GroundSamples[LegIndex].bHit && !Context.bGroundPlaneOverride)
        || (PlantSettings.LockType == EFootPlacementLockType::Unlocked) || FMath::IsNearlyZero(LegInputPose.LockAlpha))
    {
        return false;
    }

    const bool bPassesPlantDistanceCheck = LegInputPose.DistanceToPlant < PlantSettings.DistanceToGround;
    const bool bPassesSpeedCheck = LegInputPose.Speed < PlantSettings.SpeedThreshold;
    return bPassesPlantDistanceCheck && bPassesSpeedCheck;
}

float FAnimNode_PawniardFootPlacement::GetAlignmentAlpha(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose) const
{
    return FMath::Clamp(
        FMath::GetRangePct(FVector2D(PlantSettings.UnalignmentSpeedThreshold,
            PlantSettings.SpeedThreshold),
            LegInputPose.Speed),
        0.0f, 1.0f);
}

FTransform FAnimNode_PawniardFootPlacement::GetFootPivotAroundBallWS(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    const UE::Anim::FootPlacement::FLegRuntimeData::FInputPoseData& LegInputPose,
    const FTransform& LastPlantTransformWS) const
{
    const auto BallTransformWS = LegInputPose.BallTransformCS * Context.OwningComponentToWorld;

    const auto PinnedBallTransformWS = FTransform(
        BallTransformWS.GetRotation(),
        (LegInputPose.FootToBall * LastPlantTransformWS).GetLocation(),
        BallTransformWS.GetScale3D());

    return LegInputPose.BallToFoot * PinnedBallTransformWS;

}

UE::Anim::FootPlacement::FPlantResult FAnimNode_PawniardFootPlacement::FinalizeFootAlignment(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    UE::Anim::FootPlacement::FLegRuntimeData& LegData,
    const FFootPlacemenLegDefinition& LegDef,
    const FTransform& PelvisTransformCS)
{
    const auto FKPelvisToHipCS =
        LegData.InputPose.HipTransformCS.GetRelativeTransform(PelvisData.InputPose.FKTransformCS);
    const auto FinalHipTransformCS = FKPelvisToHipCS * PelvisTransformCS;
    FTransform CorrectedFootTransformCS = LegData.AlignedFootTransformRS * GetRootToComponent();
    const auto CorrectedBallLocationCS = (LegData.InputPose.FootToBall * CorrectedFootTransformCS).GetLocation();

    // avoid hyper extension - start
    const auto InputPoseHipToFoot =
        LegData.InputPose.FootTransformCS.GetLocation() - LegData.InputPose.HipTransformCS.GetLocation();
    const auto CorrectedHipToFootDir =
        (CorrectedFootTransformCS.GetLocation() - FinalHipTransformCS.GetLocation()).GetSafeNormal();

    if (!InputPoseHipToFoot.IsNearlyZero() && !CorrectedHipToFootDir.IsNearlyZero())
    {
        {
            const float DesiredExtension = InputPoseHipToFoot.Size();
            const float MaxExtension = GetMaxLimbExtension(DesiredExtension, LegData.Bones.LimbLength);
            const float CurrentExtension =
                FVector::Dist(CorrectedFootTransformCS.GetLocation(), FinalHipTransformCS.GetLocation());

            const float HyperExtensionAmount = CurrentExtension - MaxExtension;
            float HyperExtensionRemaining = HyperExtensionAmount;

            if (CurrentExtension > MaxExtension)
            {
                const bool bIsPlanted = LegData.Plant.PlantType != UE::Anim::FootPlacement::EPlantType::Unplanted;

                if (!bIsPlanted)
                {
                    // If there's any overextension and we're unplanted, target is unreachable
                    // Don't plant until we're in re-plant range
                    LegData.Plant.bCanReachTarget = false;
                }

                const bool bRecentlyUnplanted = !bIsPlanted && LegData.Plant.TimeSinceFullyUnaligned == 0.0f;
                // Try to keep the tip on spot if we're unplanting
                // Don't do this until we've reached the plant target once
                const bool bCanLiftHeel = bRecentlyUnplanted || (bIsPlanted && LegData.Plant.bCanReachTarget) || PlantSettings.bAdjustHeelBeforePlanting;
                if (bCanLiftHeel)
                {
                    // Scale this value by our FK transition alpha to not pop
                    const float MaxPullTowardsHip =  FMath::Min(LegData.Bones.FootLength, HyperExtensionRemaining) * LegData.InputPose.AlignmentAlpha;
                    HyperExtensionRemaining -= MaxPullTowardsHip;

                    const auto CorrectedFootLocationCS = CorrectedFootTransformCS.GetLocation() - MaxPullTowardsHip * CorrectedHipToFootDir;

                    // Rotate the foot to keep the toe at the same spot
                    const auto InitialFootToToe = CorrectedBallLocationCS - CorrectedFootTransformCS.GetLocation();
                    const auto CorrectedFootToToe = CorrectedBallLocationCS - CorrectedFootLocationCS;
                    FQuat DeltaSlopeRotation = FQuat::FindBetweenVectors(InitialFootToToe, CorrectedFootToToe);

                    // Rotate the foot to preserve the ball's location
                    CorrectedFootTransformCS.SetRotation(DeltaSlopeRotation * CorrectedFootTransformCS.GetRotation());
                    CorrectedFootTransformCS.NormalizeRotation();

                    // Move the foot bone closer to the hip prevent overextension
                    CorrectedFootTransformCS.SetLocation(CorrectedFootLocationCS);
                    check(!CorrectedFootTransformCS.ContainsNaN());
                }

                // Fix any remaining hyper-extension
                if (HyperExtensionRemaining > 0.0f)
                {
                    // Move IK bone towards the hip bone.
                    // preferable to slide. This causes discontinuities when the foot is no longer hyper-extended
                    FVector NotHyperextendedPlantLocation;
                    FMath::SphereDistToLine(FinalHipTransformCS.GetLocation(), MaxExtension, CorrectedFootTransformCS.GetLocation(), CorrectedHipToFootDir, NotHyperextendedPlantLocation);
                    CorrectedFootTransformCS.SetLocation(NotHyperextendedPlantLocation);
                    check(!CorrectedFootTransformCS.ContainsNaN());
                }
            }
            else
            {
                // No overextension, therefore we can reach the target, and can do future tip/ball adjustments
                LegData.Plant.bCanReachTarget = true;
            }

        }
    }

    // Next the plant is ajusted to prevent penetration with the planting plane. To do that, first the base of the plant
    // and the tip must be calculated (note that because the ground plane interpolates, this does not prevent physical penetration
    // with the geometry).
    const auto PlantPlaneCS = LegData.Plant.GetPlantPlaneCS(GetRootToComponent());
    const TOptional<float> FootDistance = UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(
        CorrectedBallLocationCS,
        PlantPlaneCS,
        Context.ApproachDirCS);
    const TOptional<float> BallDistance = UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(
        CorrectedFootTransformCS.GetLocation(),
        PlantPlaneCS,
        Context.ApproachDirCS);
    float MinDistance = FMath::Min(FootDistance.Get(0.0f), BallDistance.Get(0.0f));
    // Allow as much penetration as the source animation.
    MinDistance -= FMath::Min(0.0f, LegData.InputPose.DistanceToPlant);
    // A min distance < 0.0f means there was penetration
    if (MinDistance < 0.0f)
    {
        CorrectedFootTransformCS.AddToTranslation(MinDistance * Context.ApproachDirCS);
    }
    check(!CorrectedFootTransformCS.ContainsNaN());

    // Fix any remaining hyper-compression. Clip into the ground plane if necessary.
    // Doing this after pushing the feet out of the ground plane ensures we won't end up in awkward poses.
    {
        FVector NotHyperextendedPlantLocation;
        const float MinExtension = GetMinLimbExtension(FMath::Abs(InputPoseHipToFoot | Context.ApproachDirCS), LegData.Bones.LimbLength);

        // Offset our hip plane by min extension
        FPlane HipPlane = FPlane(FinalHipTransformCS.GetLocation() + Context.ApproachDirCS * MinExtension, Context.ApproachDirCS);
        const float DistanceToHipPlane = HipPlane.PlaneDot(CorrectedFootTransformCS.GetLocation());

        if (DistanceToHipPlane < 0.0f)
        {
            // Move foot to hip plane if we're past it.
            NotHyperextendedPlantLocation = CorrectedFootTransformCS.GetLocation() - Context.ApproachDirCS * DistanceToHipPlane;
            CorrectedFootTransformCS.SetLocation(NotHyperextendedPlantLocation);
        }
    }

    if (LegData.InputPose.DisableLeg > 0.0f)
    {
        FTransform DisabledLegTransform = LegData.InputPose.FootFKTransformCS;
        DisabledLegTransform.SetTranslation(DisabledLegTransform.GetTranslation() + GetRootToComponent().TransformVector(PelvisData.Interpolation.PelvisTranslationOffset) * (1.0-PelvisData.DisablePelvis));
        CorrectedFootTransformCS.BlendWith(DisabledLegTransform, LegData.InputPose.DisableLeg);
    }

    check(!CorrectedFootTransformCS.ContainsNaN());

    UE::Anim::FootPlacement::FPlantResult Result =
    {
        { LegData.Bones.IKIndex, CorrectedFootTransformCS },
        // { LegData.Bones.BallIndex, CorrectedBallTransformCS },
        //{ LegData.Bones.HipIndex, CorrectedHipTransformCS }
    };

    return Result;
}

void FAnimNode_PawniardFootPlacement::PreUpdate(const UAnimInstance* InAnimInstance)
{
    using namespace UE::Anim::PawniardFootPlacement;
    const auto* Mesh = InAnimInstance ? InAnimInstance->GetSkelMeshComponent() : nullptr;
    auto* World = Mesh ? Mesh->GetWorld() : nullptr;
    FSceneSnapshot NextScene;
    if (!Mesh || !World)
    {
        SceneSnapshot = MoveTemp(NextScene);
        return;
    }

    NextScene.World = World;
    const auto& ComponentTransform = Mesh->GetComponentTransform();
    NextScene.TeleportDistanceThreshold = Mesh->GetTeleportDistanceThreshold();
    NextScene.bCanEvaluate = World->IsGameWorld() || World->WorldType == EWorldType::EditorPreview;
#if WITH_EDITOR
    NextScene.bCanEvaluate |= Mesh->GetUpdateAnimationInEditor();
#endif
    NextScene.bCanEvaluate &= !ComponentTransform.ContainsNaN()
        && ComponentTransform.GetScale3D().GetMin() > SMALL_NUMBER;
    NextScene.QueryParams = FCollisionQueryParams(SCENE_QUERY_STAT(PawniardFootPlacement), false);
    if (const auto* Owner = Mesh->GetOwner())
    {
        NextScene.QueryParams.AddIgnoredActor(Owner);
        TArray<AActor*> AttachedActors;
        Owner->GetAttachedActors(AttachedActors, true, true);
        NextScene.QueryParams.AddIgnoredActors(AttachedActors);

        // Shape data and collision responses are UObject state, so capture them on the game thread.
        if (const auto* Body = Cast<UShapeComponent>(Owner->GetRootComponent());
            Body && Body->IsQueryCollisionEnabled())
        {
            NextScene.BodyCollisionShape = Body->GetCollisionShape();
            NextScene.BodyTransformWS = Body->GetComponentTransform();
            NextScene.BodyCollisionChannel = Body->GetCollisionObjectType();
            NextScene.BodyCollisionResponses = FCollisionResponseParams(Body->GetCollisionResponseToChannels());
            NextScene.bHasBodyCollision = !NextScene.BodyTransformWS.ContainsNaN()
                && !NextScene.BodyCollisionShape.IsNearlyZero()
                && !NextScene.BodyCollisionShape.GetExtent().ContainsNaN()
                && NextScene.BodyCollisionShape.GetExtent().GetMin() > SMALL_NUMBER;
        }
    }

    // Cache only environment data. Bone positions and ground queries belong to the current evaluation.
    NextScene.Supports.SetNum(LegDefinitions.Num());
    for (auto Index = 0; Index < NextScene.Supports.Num(); ++Index)
    {
        if (!GroundSamples.IsValidIndex(Index))
        {
            continue;
        }
        const auto& Previous = GroundSamples[Index];
        if (const auto* Support = Previous.Support.Get())
        {
            auto& Snapshot = NextScene.Supports[Index];
            Snapshot.Support = Previous.Support;
            Snapshot.SupportTransform = Support->GetComponentTransform();
            Snapshot.bHasSupportTransform = !Snapshot.SupportTransform.ContainsNaN();
            Snapshot.bSameSupport = Previous.bHasSupportTransform && Snapshot.bHasSupportTransform;
            Snapshot.PreviousSupportTransform = Snapshot.bSameSupport
                ? Previous.SupportTransform : Snapshot.SupportTransform;
        }
    }
    SceneSnapshot = MoveTemp(NextScene);
}

FAnimNode_PawniardFootPlacement::FAnimNode_PawniardFootPlacement()
{
}

void FAnimNode_PawniardFootPlacement::GatherDebugData(FNodeDebugData& NodeDebugData)
{
    ComponentPose.GatherDebugData(NodeDebugData);
}

void FAnimNode_PawniardFootPlacement::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
    FAnimNode_SkeletalControlBase::Initialize_AnyThread(Context);
    ResetRuntimeData();
}

void FAnimNode_PawniardFootPlacement::UpdateInternal(const FAnimationUpdateContext& Context)
{
    FAnimNode_SkeletalControlBase::UpdateInternal(Context);

    // If we just became relevant and haven't been initialized yet, then reinitialize foot placement.
    if (!bIsFirstUpdate && UpdateCounter.HasEverBeenUpdated() && !UpdateCounter.WasSynchronizedCounter(Context.AnimInstanceProxy->GetUpdateCounter()))
    {
        ResetRuntimeData();
    }
    UpdateCounter.SynchronizeWith(Context.AnimInstanceProxy->GetUpdateCounter());

    if (bReset && !bResetWasRequested)
    {
        ResetRuntimeData();
    }
    bResetWasRequested = bReset;
    CachedDeltaTime += Context.GetDeltaTime();
}

void FAnimNode_PawniardFootPlacement::EvaluateSkeletalControl_AnyThread(
    FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
    SCOPE_CYCLE_COUNTER(STAT_PawniardFootPlacement_Eval);
    const auto DeltaTime = CachedDeltaTime;
    CachedDeltaTime = 0.0f;
    if (!SceneSnapshot.bCanEvaluate || SceneSnapshot.Supports.Num() != LegsData.Num()
        || DeltaTime <= SMALL_NUMBER || Output.AnimInstanceProxy->GetComponentTransform().ContainsNaN()
        || Output.AnimInstanceProxy->GetComponentTransform().GetScale3D().GetMin() <= SMALL_NUMBER)
    {
        return;
    }

    if (!bIsFirstUpdate)
    {
        const auto& CurrentTransform = Output.AnimInstanceProxy->GetComponentTransform();
        const auto Threshold = SceneSnapshot.TeleportDistanceThreshold;
        if ((Threshold > 0.0f && FVector::DistSquared(CurrentTransform.GetLocation(),
                CharacterData.ComponentTransformWS.GetLocation()) > FMath::Square(Threshold))
            || !CurrentTransform.GetScale3D().Equals(CharacterData.ComponentTransformWS.GetScale3D()))
        {
            ResetRuntimeData();
        }
    }

    UE::Anim::PawniardFootPlacement::FEvaluationContext Context(Output, SceneSnapshot, UpDirectionWS, MaxGroundSlopeAngle, DeltaTime);
    if (!bIsFirstUpdate && FVector::DotProduct(-Context.ApproachDirWS, PreviousUpWS) < 0.999f)
    {
        ResetRuntimeData();
    }
    PreviousUpWS = -Context.ApproachDirWS;
    GatherPelvisDataFromInputs(Context);
    const auto ReferenceWS = Context.OwningComponentToWorld.TransformPosition(
        PelvisData.InputPose.IKRootTransformCS.GetLocation());
    // Reference support may span a ledge; foot contacts resolve their own surfaces separately.
    const auto ReferenceGround = UE::Anim::PawniardFootPlacement::FindPlantPlane(
        Context, TraceSettings, ReferenceWS, true, nullptr, false);
    Context.GroundLocation = ReferenceGround.bHit ? ReferenceGround.PositionWS : ReferenceWS;
    Context.GroundNormal = ReferenceGround.bHit ? ReferenceGround.NormalWS : -Context.ApproachDirWS;
    // Support distances are in world centimeters; mesh scale must not shrink the grounding tolerance.
    const auto GroundTolerance = FMath::Max(0.0f, AutoGroundDistance)
        * (CharacterData.bIsOnGround ? 1.5f : 1.0f);
    Context.bGrounded = ReferenceGround.bHit
        && FMath::Abs(FVector::DotProduct(ReferenceWS - ReferenceGround.PositionWS, -Context.ApproachDirWS))
            <= GroundTolerance;
    if (bOverrideGroundPlane && !GroundNormalWS.IsNearlyZero() && !GroundNormalWS.ContainsNaN()
        && !GroundLocationWS.ContainsNaN())
    {
        Context.GroundLocation = GroundLocationWS;
        Context.GroundNormal = GroundNormalWS.GetSafeNormal();
        Context.bGroundPlaneOverride = FVector::DotProduct(Context.GroundNormal, -Context.ApproachDirWS) > SMALL_NUMBER;
        if (Context.bGroundPlaneOverride && !bOverrideGrounded)
        {
            const auto Distance = UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(
                ReferenceWS, FPlane(Context.GroundLocation, Context.GroundNormal), Context.ApproachDirWS);
            Context.bGrounded = FMath::Abs(Distance.Get(BIG_NUMBER))
                <= GroundTolerance;
        }
    }
    // The reference may cross a ledge while either foot still supports the character.
    auto bHasFootSupport = false;
    auto ClosestSupportDistance = BIG_NUMBER;
    GroundSamples.SetNum(LegsData.Num());
    for (auto Index = 0; Index < LegsData.Num(); ++Index)
    {
        GatherLegDataFromInputs(Context, LegsData[Index], LegDefinitions[Index]);
        const auto& Leg = LegsData[Index];
        const auto& Support = SceneSnapshot.Supports[Index];
        auto QueryPointWS = Context.OwningComponentToWorld.TransformPosition(Leg.InputPose.FootTransformCS.GetLocation());
        if (!bIsFirstUpdate && Leg.Plant.PlantType != UE::Anim::FootPlacement::EPlantType::Unplanted)
        {
            QueryPointWS = Leg.UnalignedFootTransformWS.GetLocation();
            if (Support.bSameSupport)
            {
                QueryPointWS = Support.SupportTransform.TransformPosition(
                    Support.PreviousSupportTransform.InverseTransformPosition(QueryPointWS));
            }
        }
        // Establish contact from this pose before deciding whether the leg may lock.
        GroundSamples[Index] = UE::Anim::PawniardFootPlacement::FindPlantPlane(
            Context, TraceSettings, QueryPointWS, false, &Support);

        if (Context.bGrounded || bOverrideGrounded || Leg.InputPose.DisableLeg >= 1.0f)
        {
            continue;
        }

        // Locked IK targets can remain on the ground after takeoff; only the current FK pose proves support.
        const auto FootFKPointWS = Context.OwningComponentToWorld.TransformPosition(
            Leg.InputPose.FootFKTransformCS.GetLocation());
        const auto BallFKPointWS = Context.OwningComponentToWorld.TransformPosition(
            (Leg.InputPose.FootToBall * Leg.InputPose.FootFKTransformCS).GetLocation());
        auto SupportLocationWS = Context.GroundLocation;
        auto SupportNormalWS = Context.GroundNormal;
        if (!Context.bGroundPlaneOverride)
        {
            auto SupportSample = GroundSamples[Index];
            if (!SupportSample.QueryPointWS.Equals(FootFKPointWS, KINDA_SMALL_NUMBER))
            {
                SupportSample = UE::Anim::PawniardFootPlacement::FindPlantPlane(
                    Context, TraceSettings, FootFKPointWS, false, &Support);
            }
            if (!SupportSample.bHit)
            {
                continue;
            }
            SupportLocationWS = SupportSample.PositionWS;
            SupportNormalWS = SupportSample.NormalWS;
        }

        const auto SupportPlaneWS = FPlane(SupportLocationWS, SupportNormalWS);
        const auto FootDistance = UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(
            FootFKPointWS, SupportPlaneWS, Context.ApproachDirWS);
        const auto BallDistance = UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(
            BallFKPointWS, SupportPlaneWS, Context.ApproachDirWS);
        const auto SupportDistance = FMath::Min(FMath::Abs(FootDistance.Get(BIG_NUMBER)),
            FMath::Abs(BallDistance.Get(BIG_NUMBER)));
        if (SupportDistance <= GroundTolerance && SupportDistance < ClosestSupportDistance)
        {
            bHasFootSupport = true;
            ClosestSupportDistance = SupportDistance;
            if (!Context.bGroundPlaneOverride)
            {
                Context.GroundLocation = SupportLocationWS;
                Context.GroundNormal = SupportNormalWS;
            }
        }
    }
    Context.bGrounded |= bHasFootSupport;
    if (!Context.bGrounded && !bOverrideGrounded && !Context.bGroundPlaneOverride && TraceSettings.bEnabled)
    {
        // The body can still be supported by a ledge while both FK feet await IK adjustment.
        const auto BodySupport = UE::Anim::PawniardFootPlacement::FindBodySupport(Context, GroundTolerance);
        if (BodySupport.bHit)
        {
            Context.bGrounded = true;
            Context.GroundLocation = BodySupport.PositionWS;
            Context.GroundNormal = BodySupport.NormalWS;
        }
    }
    TRACE_ANIM_NODE_VALUE(Output, TEXT("AutoGroundToleranceWS"), GroundTolerance);
    TRACE_ANIM_NODE_VALUE(Output, TEXT("AutomaticGrounded"), Context.bGrounded);
    TRACE_ANIM_NODE_VALUE(Output, TEXT("OverrideGrounded"), bOverrideGrounded);
    if (bOverrideGrounded)
    {
        Context.bGrounded = bGrounded;
    }
    if (!Context.bGrounded)
    {
        for (auto& Leg : LegsData)
        {
            Leg.InputPose.LockAlpha = 0.0f;
            Leg.InputPose.AlignmentAlpha = 0.0f;
        }
    }

    PlantRuntimeSettings.UnplantRadiusSqrd = FMath::Square(PlantSettings.UnplantRadius * Context.WorldUnitsPerComponentUnit);
    PlantRuntimeSettings.ReplantRadiusSqrd = PlantRuntimeSettings.UnplantRadiusSqrd * FMath::Square(PlantSettings.ReplantRadiusRatio);
    PlantRuntimeSettings.CosHalfUnplantAngle = FMath::Cos(FMath::DegreesToRadians(PlantSettings.UnplantAngle * 0.5f));
    PlantRuntimeSettings.CosHalfReplantAngle = FMath::Cos(FMath::DegreesToRadians(
        PlantSettings.UnplantAngle * PlantSettings.ReplantAngleRatio * 0.5f));

    ProcessComponentState(Context);
    CalculateFootMidpoint(Context, LegsData, PelvisData.InputPose.FootMidpointCS);
    for (auto& Leg : LegsData)
    {
        ProcessFootAlignment(Context, Leg);
    }

    auto PelvisTransformCS = SolvePelvis(Context);
    if (PelvisSettings.bEnableInterpolation)
    {
        const auto& RootCS = GetRootToComponent();
        PelvisTransformCS = UpdatePelvisInterpolationRootSpace(Context,
            PelvisTransformCS.GetRelativeTransform(RootCS)) * RootCS;
    }
    PelvisData.DisablePelvis = FMath::Clamp(Output.Curve.Get(PelvisSettings.DisablePelvisCurveName), 0.0f, 1.0f);
    PelvisTransformCS.BlendWith(PelvisData.InputPose.FKTransformCS, PelvisData.DisablePelvis);
    OutBoneTransforms.Add(FBoneTransform(PelvisData.Bones.FkBoneIndex, PelvisTransformCS));

    if (InterpolationSettings.bSmoothRootBone && PelvisData.Bones.FkBoneIndex.GetInt() != 0)
    {
        auto RootTransform = Output.Pose.GetComponentSpaceTransform(FCompactPoseBoneIndex(0));
        RootTransform.AddToTranslation(PelvisTransformCS.GetLocation() - PelvisData.InputPose.FKTransformCS.GetLocation());
        OutBoneTransforms.Add(FBoneTransform(FCompactPoseBoneIndex(0), RootTransform));
    }
    for (auto Index = 0; Index < LegsData.Num(); ++Index)
    {
        OutBoneTransforms.Add(FinalizeFootAlignment(Context, LegsData[Index], LegDefinitions[Index], PelvisTransformCS).FootTranformCS);
    }
    if (OutBoneTransforms.ContainsByPredicate([](const auto& Bone) { return Bone.Transform.ContainsNaN(); }))
    {
        OutBoneTransforms.Reset();
        ResetRuntimeData();
        return;
    }
    OutBoneTransforms.Sort(FCompareBoneTransformIndex());
    bIsFirstUpdate = false;
}

bool FAnimNode_PawniardFootPlacement::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
    return bBonesValid && !LegsData.IsEmpty() && LegsData.Num() == LegDefinitions.Num();
}

void FAnimNode_PawniardFootPlacement::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
    ResetRuntimeData();
    bBonesValid = !LegDefinitions.IsEmpty();
    PelvisBone.Initialize(RequiredBones);
    IKFootRootBone.Initialize(RequiredBones);
    PelvisData.Bones.FkBoneIndex = PelvisBone.GetCompactPoseIndex(RequiredBones);
    PelvisData.Bones.IkBoneIndex = IKFootRootBone.GetCompactPoseIndex(RequiredBones);
    bBonesValid &= PelvisData.Bones.FkBoneIndex.IsValid() && PelvisData.Bones.IkBoneIndex.IsValid()
        && PelvisData.Bones.FkBoneIndex != PelvisData.Bones.IkBoneIndex;
    TSet<int32> OutputIndices;
    OutputIndices.Add(PelvisData.Bones.FkBoneIndex.GetInt());
    if (InterpolationSettings.bSmoothRootBone)
    {
        bBonesValid &= PelvisData.Bones.FkBoneIndex.GetInt() != 0;
        OutputIndices.Add(0);
    }

    for (auto Index = 0; Index < LegsData.Num(); ++Index)
    {
        auto& Definition = LegDefinitions[Index];
        auto& Leg = LegsData[Index];
        Definition.FKFootBone.Initialize(RequiredBones);
        Definition.IKFootBone.Initialize(RequiredBones);
        Definition.BallBone.Initialize(RequiredBones);
        Leg.Bones.FKIndex = Definition.FKFootBone.GetCompactPoseIndex(RequiredBones);
        Leg.Bones.IKIndex = Definition.IKFootBone.GetCompactPoseIndex(RequiredBones);
        Leg.Bones.BallIndex = Definition.BallBone.GetCompactPoseIndex(RequiredBones);
        if (!Leg.Bones.FKIndex.IsValid() || !Leg.Bones.IKIndex.IsValid()
            || !Leg.Bones.BallIndex.IsValid() || Definition.NumBonesInLimb < 1
            || Leg.Bones.FKIndex == Leg.Bones.IKIndex || OutputIndices.Contains(Leg.Bones.IKIndex.GetInt()))
        {
            bBonesValid = false;
            continue;
        }
        OutputIndices.Add(Leg.Bones.IKIndex.GetInt());
        auto HipIndex = Leg.Bones.FKIndex;
        for (auto Bone = 0; Bone < Definition.NumBonesInLimb && HipIndex.IsValid(); ++Bone)
        {
            HipIndex = RequiredBones.GetParentBoneIndex(HipIndex);
        }
        Leg.Bones.HipIndex = HipIndex;
        bBonesValid &= HipIndex.IsValid();
    }
#if WITH_EDITOR
    if (!bBonesValid)
    {
        AddValidationVisualWarning(NSLOCTEXT("PawniardFootPlacement", "InvalidBones",
            "Configure valid pelvis, IK foot root, FK foot, IK foot and ball bones with distinct output bones and a complete leg chain."));
    }
#endif
}

void FAnimNode_PawniardFootPlacement::GatherPelvisDataFromInputs(const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context)
{
    PelvisData.InputPose.FKTransformCS =
        Context.CSPContext.Pose.GetComponentSpaceTransform(PelvisData.Bones.FkBoneIndex);
    PelvisData.InputPose.IKRootTransformCS =
        Context.CSPContext.Pose.GetComponentSpaceTransform(PelvisData.Bones.IkBoneIndex);

    PelvisData.InputPose.RootTransformCS =
        Context.CSPContext.Pose.GetComponentSpaceTransform(FCompactPoseBoneIndex(0));

    PelvisData.MaxOffsetSqrd = PelvisSettings.MaxOffset * PelvisSettings.MaxOffset;
}

void FAnimNode_PawniardFootPlacement::GatherLegDataFromInputs(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    UE::Anim::FootPlacement::FLegRuntimeData& LegData, const FFootPlacemenLegDefinition& LegDef)
{
    const auto PreviousBallCS = (LegData.InputPose.FootToBall * LegData.InputPose.FootFKTransformCS).GetLocation();
    auto& Input = LegData.InputPose;
    Input.FootFKTransformCS = Context.CSPContext.Pose.GetComponentSpaceTransform(LegData.Bones.FKIndex);
    const auto BallFK = Context.CSPContext.Pose.GetComponentSpaceTransform(LegData.Bones.BallIndex);
    Input.FootTransformCS = bUseInputIKTargets
        ? Context.CSPContext.Pose.GetComponentSpaceTransform(LegData.Bones.IKIndex) : Input.FootFKTransformCS;
    Input.HipTransformCS = Context.CSPContext.Pose.GetComponentSpaceTransform(LegData.Bones.HipIndex);
    Input.BallToFoot = Input.FootFKTransformCS.GetRelativeTransform(BallFK);
    Input.FootToBall = BallFK.GetRelativeTransform(Input.FootFKTransformCS);
    Input.BallTransformCS = Input.FootToBall * Input.FootTransformCS;

    // Use posed chain lengths so retargeted bone scales do not change the solver's reach.
    const auto& Bones = Context.CSPContext.Pose.GetPose().GetBoneContainer();
    LegData.Bones.LimbLength = 0.0f;
    auto BoneIndex = LegData.Bones.FKIndex;
    for (auto Bone = 0; Bone < LegDef.NumBonesInLimb; ++Bone)
    {
        const auto ParentIndex = Bones.GetParentBoneIndex(BoneIndex);
        LegData.Bones.LimbLength += FVector::Distance(
            Context.CSPContext.Pose.GetComponentSpaceTransform(BoneIndex).GetLocation(),
            Context.CSPContext.Pose.GetComponentSpaceTransform(ParentIndex).GetLocation());
        BoneIndex = ParentIndex;
    }
    LegData.Bones.FootLength = FVector::Distance(BallFK.GetLocation(), Input.FootFKTransformCS.GetLocation());

    if (bIsFirstUpdate)
    {
        const auto& RootCS = GetRootToComponent();
        LegData.AlignedFootTransformRS = Input.FootTransformCS.GetRelativeTransform(RootCS);
        LegData.AlignedFootTransformWS = Input.FootTransformCS * Context.OwningComponentToWorld;
        LegData.UnalignedFootTransformRS = LegData.AlignedFootTransformRS;
        LegData.UnalignedFootTransformWS = LegData.AlignedFootTransformWS;
        LegData.Plant.PlantPlaneRS = FPlane(
            RootCS.InverseTransformPosition(PelvisData.InputPose.IKRootTransformCS.GetLocation()),
            RootCS.InverseTransformVectorNoScale(-Context.ApproachDirCS).GetSafeNormal());
    }

    auto AutomaticSpeed = 0.0f;
    if (!bIsFirstUpdate)
    {
        auto PreviousBallWS = CharacterData.ComponentTransformWS.TransformPosition(PreviousBallCS);
        const auto& Sample = Context.Scene.Supports[LegData.Idx];
        if (Sample.bSameSupport)
        {
            PreviousBallWS = Sample.SupportTransform.TransformPosition(
                Sample.PreviousSupportTransform.InverseTransformPosition(PreviousBallWS));
        }
        auto DeltaWS = Context.OwningComponentToWorld.TransformPosition(BallFK.GetLocation()) - PreviousBallWS;
        if (bOverrideVelocity)
        {
            DeltaWS += VelocityWS * Context.UpdateDeltaTime
                - (Context.OwningComponentToWorld.GetLocation() - CharacterData.ComponentTransformWS.GetLocation());
        }
        // Mesh displacement already includes applied root motion; adding its attribute again would double-count it.
        AutomaticSpeed = DeltaWS.Size() / (Context.UpdateDeltaTime * Context.WorldUnitsPerComponentUnit);
    }
    bool bValidCurve = false;
    Input.Speed = PlantSpeedMode == EWarpingEvaluationMode::Manual
        ? Context.CSPContext.Curve.Get(LegData.SpeedCurveName, bValidCurve, AutomaticSpeed) : AutomaticSpeed;
    Input.Speed = FMath::Max(0.0f, Input.Speed);
    Input.DisableLeg = FMath::Clamp(Context.CSPContext.Curve.Get(LegData.DisableLegCurveName), 0.0f, 1.0f);
    Input.LockAlpha = FMath::Clamp(1.0f - Context.CSPContext.Curve.Get(LegData.DisableLockCurveName), 0.0f, 1.0f);
    Input.DistanceToPlant = CalcTargetPlantPlaneDistance(Context, Input);
    Input.AlignmentAlpha = GetAlignmentAlpha(Context, Input);
}

void FAnimNode_PawniardFootPlacement::CalculateFootMidpoint(const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context, TConstArrayView<UE::Anim::FootPlacement::FLegRuntimeData> InLegsData, FVector& OutMidpoint) const
{
    int32 NumLegs = LegsData.Num();
    OutMidpoint = FVector::ZeroVector;
    for (const UE::Anim::FootPlacement::FLegRuntimeData& LegData : InLegsData)
    {
        OutMidpoint += LegData.InputPose.FootTransformCS.GetLocation() / NumLegs;
    }
}

void FAnimNode_PawniardFootPlacement::ProcessComponentState(const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context)
{
    const auto LastComponentLocationWS = bIsFirstUpdate
        ? Context.OwningComponentToWorld.GetLocation()
        : CharacterData.ComponentTransformWS.GetLocation();

    if (bIsFirstUpdate)
    {
        CharacterData.SmoothCapsuleGroundNormalWS = -Context.ApproachDirWS;
        CharacterData.SmoothCapsuleGroundNormalSpringState.Reset();
    }

    CharacterData.ComponentTransformWS = Context.OwningComponentToWorld;
    const auto ComponentLocationWS = CharacterData.ComponentTransformWS.GetLocation();

    const bool bWasOnGround = CharacterData.bIsOnGround;
    CharacterData.bIsOnGround = Context.bGrounded;

    CharacterData.ComponentMoveDeltaWS = FVector::ZeroVector;
    bool bOnGround =  !PelvisSettings.bDisablePelvisOffsetInAir || (CharacterData.bIsOnGround && bWasOnGround);
    if (bOnGround && PelvisSettings.ActorMovementCompensationMode != EActorMovementCompensationMode::ComponentSpace)
    {
        FVector OwningComponentAdjustedLastLocationWS;
        if (PelvisSettings.ActorMovementCompensationMode == EActorMovementCompensationMode::SuddenMotionOnly)
        {
            FQuat SlopeDelta = FQuat::FindBetweenNormals(CharacterData.SmoothCapsuleGroundNormalWS, Context.GroundNormal);
            SlopeDelta = UKismetMathLibrary::QuaternionSpringInterp(
                                                                FQuat::Identity,
                                                                SlopeDelta,
                                                                CharacterData.SmoothCapsuleGroundNormalSpringState,
                                                                InterpolationSettings.FloorAngularStiffness,
                                                                1.0f,
                                                                Context.UpdateDeltaTime,
                                                                1.0f,
                                                                0.0f);
            CharacterData.SmoothCapsuleGroundNormalWS = SlopeDelta.RotateVector(CharacterData.SmoothCapsuleGroundNormalWS);

            // Keep sudden mesh height changes from popping the pelvis.
            const auto ComponentFloorNormalWS = CharacterData.SmoothCapsuleGroundNormalWS;
            OwningComponentAdjustedLastLocationWS =
                (FMath::Abs(Context.ApproachDirWS | ComponentFloorNormalWS) > DELTA) ?
                UE::Anim::PawniardFootPlacement::PointDirectionPlaneIntersection(
                    ComponentLocationWS,
                    Context.ApproachDirWS,
                    FPlane(LastComponentLocationWS, ComponentFloorNormalWS)) :
                ComponentLocationWS;
        }
        else //if (PelvisSettings.ActorMovementCompensationMode == EActorMovementCompensationMode::WorldSpace)
        {
            // Compensate for all moves
            OwningComponentAdjustedLastLocationWS = LastComponentLocationWS;
        }

        // Only compensate vertical motion
        const auto ComponentMoveOffsetWS =
            FVector::DotProduct(ComponentLocationWS - OwningComponentAdjustedLastLocationWS - BaseTranslationDelta, -Context.ApproachDirWS) * -Context.ApproachDirWS;

        CharacterData.ComponentMoveDeltaWS -= ComponentMoveOffsetWS;
        if (!ComponentMoveOffsetWS.IsNearlyZero(KINDA_SMALL_NUMBER))
        {
            const auto ComponentMoveOffsetCS =
                Context.OwningComponentToWorld.InverseTransformVector(ComponentMoveOffsetWS);

            // Offseting our interpolator lets it smoothly solve sudden capsule deltas, instead of following it and pop
            const auto OffsetRS = GetRootToComponent().InverseTransformVector(ComponentMoveOffsetCS);
            PelvisData.Interpolation.PelvisTranslationOffset -= OffsetRS;

            for (auto& LegData : LegsData)
            {
                // Also offset our foot plant plane interpolators by this same delta.
                LegData.Plant.PlantPlaneRS = LegData.Plant.PlantPlaneRS.TranslateBy(-OffsetRS);
            }
        }
    }

    {
        CharacterData.CharacterVelocityWS = bOverrideVelocity ? VelocityWS
            : (ComponentLocationWS - LastComponentLocationWS) / Context.UpdateDeltaTime;

        // Track actual component motion so slopes and external movement are included.
        const auto ComponentMoveOffsetWS =
            (ComponentLocationWS - LastComponentLocationWS);
        CharacterData.ComponentMoveDeltaWS += ComponentMoveOffsetWS;
    }

    TRACE_ANIM_NODE_VALUE(Context.CSPContext, TEXT("Grounded"), CharacterData.bIsOnGround);
    TRACE_ANIM_NODE_VALUE(Context.CSPContext, TEXT("WasGrounded"), bWasOnGround);
    TRACE_ANIM_NODE_VALUE(Context.CSPContext, TEXT("CompensationEnabled"),
        bOnGround && PelvisSettings.ActorMovementCompensationMode != EActorMovementCompensationMode::ComponentSpace);
    TRACE_ANIM_NODE_VALUE(Context.CSPContext, TEXT("ComponentDeltaUpWS"),
        static_cast<float>(FVector::DotProduct(ComponentLocationWS - LastComponentLocationWS, -Context.ApproachDirWS)));
    TRACE_ANIM_NODE_VALUE(Context.CSPContext, TEXT("CompensationUpWS"),
        static_cast<float>(FVector::DotProduct(
            CharacterData.ComponentMoveDeltaWS - (ComponentLocationWS - LastComponentLocationWS), -Context.ApproachDirWS)));
}

void FAnimNode_PawniardFootPlacement::ProcessFootAlignment(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    UE::Anim::FootPlacement::FLegRuntimeData& LegData)
{
    auto& InputPose = LegData.InputPose;
    auto& Interpolation = LegData.Interpolation;
    auto& Plant = LegData.Plant;

    const auto& Support = Context.Scene.Supports[LegData.Idx];
    if (!bIsFirstUpdate && Support.bSameSupport)
    {
        LegData.AlignedFootTransformWS = LegData.AlignedFootTransformWS.GetRelativeTransform(Support.PreviousSupportTransform) * Support.SupportTransform;
        LegData.UnalignedFootTransformWS = LegData.UnalignedFootTransformWS.GetRelativeTransform(Support.PreviousSupportTransform) * Support.SupportTransform;
    }
    else if (!bIsFirstUpdate && Plant.PlantType != UE::Anim::FootPlacement::EPlantType::Unplanted)
    {
        Plant.PlantType = UE::Anim::FootPlacement::EPlantType::Unplanted;
        Plant.bWantsToPlant = false;
    }

    const auto& RootToComponent = GetRootToComponent();
    if (PlantSettings.bReconstructWorldPlantFromVelocity)
    {
        // Last frame's plant in root space minus our move delta from character velocity.
        LegData.AlignedFootTransformWS = LegData.AlignedFootTransformRS * RootToComponent * Context.OwningComponentToWorld;
        LegData.AlignedFootTransformWS.AddToTranslation(-CharacterData.CharacterVelocityWS * Context.UpdateDeltaTime);

        LegData.UnalignedFootTransformWS = LegData.UnalignedFootTransformRS * RootToComponent * Context.OwningComponentToWorld;
        LegData.UnalignedFootTransformWS.AddToTranslation(-CharacterData.CharacterVelocityWS * Context.UpdateDeltaTime);
    }
    else
    {
        LegData.AlignedFootTransformWS.AddToTranslation(-BaseTranslationDelta);
        LegData.UnalignedFootTransformWS.AddToTranslation(-BaseTranslationDelta);
    }

    const auto InputPoseFootTransformWS = InputPose.FootTransformCS * Context.OwningComponentToWorld;
    const auto LastAlignedFootTransformWS = LegData.AlignedFootTransformWS;
    const auto LastUnalignedFootTransformWS = LegData.UnalignedFootTransformWS;
    const auto InputPoseFootTransformRS = InputPose.FootTransformCS.GetRelativeTransform(RootToComponent);

    Plant.LastPlantType = Plant.PlantType;
    DeterminePlantType(
        Context,
        LegData.Idx,
        InputPoseFootTransformWS,
        LastAlignedFootTransformWS,
        Plant,
        InputPose);

    const bool bIsPlanted = Plant.PlantType != UE::Anim::FootPlacement::EPlantType::Unplanted;

    if (bIsPlanted)
    {
        FTransform CurrentPlantedTransformWS;

        switch (PlantSettings.LockType)
        {
        case EFootPlacementLockType::Unlocked:
            break;
        case EFootPlacementLockType::PivotAroundBall:
        {
            // Figure out the correct foot transform that keeps the ball in place
            CurrentPlantedTransformWS = GetFootPivotAroundBallWS(Context, InputPose, LastUnalignedFootTransformWS);
        }
            break;
        case EFootPlacementLockType::PivotAroundAnkle:
        {
            // Use the location only
            CurrentPlantedTransformWS = InputPoseFootTransformWS;
            CurrentPlantedTransformWS.SetLocation(LastUnalignedFootTransformWS.GetLocation());
        }
            break;
        case EFootPlacementLockType::LockRotation:
        {
            // We use the unaligned foot instead of the aligned one
            // Because we will adjust roll and twist dynamically
            CurrentPlantedTransformWS = LastUnalignedFootTransformWS;
        }
            break;
        default: check(false); break; //not implemented
        }

        const auto PlantedFootTransformCS =
            CurrentPlantedTransformWS * Context.OwningComponentToWorld.Inverse();
        FTransform PlantedFootTransformRS = PlantedFootTransformCS.GetRelativeTransform(RootToComponent);

        // The locked transform is aligned to the ground. Conserve the input pose's ground alignment
        const auto AlignedBoneLocationRS = PlantedFootTransformRS.GetLocation();
        const auto InputPosePlantPlane = FPlane(InputPoseFootTransformRS.GetLocation(), RootToComponent.InverseTransformVectorNoScale(Context.ApproachDirCS).GetSafeNormal());
        const auto UnalignedBoneLocationRS = FVector::PointPlaneProject(AlignedBoneLocationRS, InputPosePlantPlane);

        PlantedFootTransformRS.SetLocation(UnalignedBoneLocationRS);

        // Get the offset relative to the initial foot transform
        // Reset interpolation
        Interpolation.UnalignedFootOffset =
            InputPoseFootTransformRS.GetRelativeTransformReverse(PlantedFootTransformRS);
        Interpolation.PlantOffsetTranslationSpringState.Reset();
        Interpolation.PlantOffsetRotationSpringState.Reset();

        // If we planted, we're fully unaligned
        Plant.TimeSinceFullyUnaligned = 0.0f;
    }
    else
    {
        // No plant, so we interpolate the offset out
        Interpolation.UnalignedFootOffset =
            UpdatePlantOffsetInterpolation(Context, Interpolation);

        // If we're unplanted, we know we're fully unaligned the first time we hit zero alignment alpha.
        if (Plant.TimeSinceFullyUnaligned > 0.0f || FMath::IsNearlyZero(InputPose.AlignmentAlpha))
        {
            Plant.TimeSinceFullyUnaligned += Context.UpdateDeltaTime;
        }
    }

    // If replant radius is the same as unplant radius, clamp the location and slide
    if (PlantSettings.ReplantRadiusRatio >= 1.0f)
    {
        const auto ClampedTransltionOffset = Interpolation.UnalignedFootOffset.GetLocation().GetClampedToMaxSize(PlantSettings.UnplantRadius);
        Interpolation.UnalignedFootOffset.SetLocation(ClampedTransltionOffset);
    }

    // If replant angle is the same as unplant angle, clamp the angle and slide
    if (PlantSettings.ReplantAngleRatio >= 1.0f)
    {
        FQuat ClampedRotationOffset = Interpolation.UnalignedFootOffset.GetRotation();
        ClampedRotationOffset.Normalize();
        ClampedRotationOffset = ClampedRotationOffset.W < 0.0 ? -ClampedRotationOffset : ClampedRotationOffset;

        FVector OffsetAxis;
        float OffsetAngle;
        ClampedRotationOffset.ToAxisAndAngle(OffsetAxis, OffsetAngle);

        const float MaxAngle = FMath::DegreesToRadians(PlantSettings.UnplantAngle);
        if (FMath::Abs(OffsetAngle) > MaxAngle)
        {
            ClampedRotationOffset = FQuat(OffsetAxis, MaxAngle);
        }
        Interpolation.UnalignedFootOffset.SetRotation(ClampedRotationOffset);
    }

    FTransform FootUnalignedTransformRS = InputPose.FootTransformCS.GetRelativeTransform(RootToComponent) * Interpolation.UnalignedFootOffset;
    if (PlantSettings.SeparatingDistance > 0.0f)
    {
        // Prevent the feet from crossing by enforcing a set distance from a plane at the midpoint between all feet
        FVector FootUnalignedLocationRS = FootUnalignedTransformRS.GetLocation();
        const auto MidPointToFoot = (InputPose.FootTransformCS.GetLocation() - PelvisData.InputPose.FootMidpointCS);
        const auto PlaneNormal = FVector::VectorPlaneProject(MidPointToFoot, Context.ApproachDirCS).GetSafeNormal();
        const auto PlaneCenter = PelvisData.InputPose.FootMidpointCS + PlaneNormal * PlantSettings.SeparatingDistance;
        const auto SeparatingPlane = FPlane(PlaneCenter, PlaneNormal);

        const TOptional<float> DistanceToSeparatingPlane =
            UE::Anim::PawniardFootPlacement::GetDistanceToPlaneAlongDirection(FootUnalignedLocationRS, SeparatingPlane, -PlaneNormal);

        if (Plant.PlantType == UE::Anim::FootPlacement::EPlantType::Unplanted)
        {
            FVector SeparatingPlaneOffset(FVector::ZeroVector);
            if (DistanceToSeparatingPlane.Get(0.0f) < 0.0f)
            {
                SeparatingPlaneOffset = -PlaneNormal * DistanceToSeparatingPlane.GetValue() ;
            }

            if (InterpolationSettings.bEnableSeparationInterpolation)
            {
                Interpolation.SeparatingPlaneOffset = UKismetMathLibrary::VectorSpringInterp(
                    Interpolation.SeparatingPlaneOffset, SeparatingPlaneOffset, Interpolation.SeparatingPlaneOffsetSpringState,
                    InterpolationSettings.FloorLinearStiffness,
                    InterpolationSettings.FloorLinearDamping,
                    Context.UpdateDeltaTime, 1.0f, 0.0f);
            }
            else
            {
                Interpolation.SeparatingPlaneOffset = SeparatingPlaneOffset;
            }

            FootUnalignedLocationRS  += Interpolation.SeparatingPlaneOffset;
            FootUnalignedTransformRS.SetLocation(FootUnalignedLocationRS);
        }
        else
        {
            Interpolation.SeparatingPlaneOffset = FVector::ZeroVector;
            Interpolation.SeparatingPlaneOffsetSpringState.Reset();
        }

    }

    FTransform BlendedUnalignedTransformRS;
    {
        // Blend the component-space input pose, with the unaligned foot-locked transform.
        // Allow ground alignment to continue with this blended result.
        // When the lock alpha reaches 0, we will automatically unlock the foot.
        BlendedUnalignedTransformRS.Blend(InputPoseFootTransformRS, FootUnalignedTransformRS, InputPose.LockAlpha);
        LegData.UnalignedFootTransformRS = BlendedUnalignedTransformRS;
        LegData.UnalignedFootTransformWS = BlendedUnalignedTransformRS * RootToComponent * Context.OwningComponentToWorld;
    }

    const auto ComponentToWorldInv = Context.OwningComponentToWorld.Inverse();

    // find the smooth plant plane
    FPlane PlantPlaneWS = Plant.GetPlantPlaneWS(RootToComponent, Context.OwningComponentToWorld);
    UpdatePlantingPlaneInterpolation(Context, LegData.Idx, LegData.UnalignedFootTransformWS,
                                    LastAlignedFootTransformWS,
                                    InputPose.AlignmentAlpha,
                                    PlantPlaneWS,
                                    InputPose,
                                    Interpolation);
    const auto PlantPlaneCS = PlantPlaneWS.TransformBy(ComponentToWorldInv.ToMatrixWithScale());
    Plant.PlantPlaneRS = PlantPlaneCS.TransformBy(RootToComponent.Inverse().ToMatrixWithScale());

    // This will adjust UnalignedFootTransformWS to make it match the required distance to the plant plane along the
    // approach direction, not the plane normal
    LegData.AlignedFootTransformWS = LegData.UnalignedFootTransformWS;
    AlignPlantToGround(Context, PlantPlaneWS, InputPose, LegData.AlignedFootTransformWS, Plant.TwistCorrection);

    const auto AlignedFootTransformCS =	LegData.AlignedFootTransformWS * ComponentToWorldInv;
    LegData.AlignedFootTransformRS = AlignedFootTransformCS.GetRelativeTransform(RootToComponent);
    LegData.AlignedFootTransformWS = AlignedFootTransformCS * Context.OwningComponentToWorld;
}

const FTransform& FAnimNode_PawniardFootPlacement::GetRootToComponent() const
{
    return PelvisData.InputPose.RootTransformCS;
}

FTransform FAnimNode_PawniardFootPlacement::SolvePelvis(const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context)
{
    using namespace UE::Anim::FootPlacement;

    // Rebalance the pelvis before calculating its desired height
    FTransform RebalancedPelvisTransform  = PelvisData.InputPose.FKTransformCS;
    FVector PelvisOffsetDelta = FVector::ZeroVector;
    if (PelvisSettings.HorizontalRebalancingWeight)
    {
        const int32 NumLegs = LegsData.Num();
        FVector OffsetAverage = FVector::ZeroVector;
        for (const auto& LegData : LegsData)
        {
            const auto LegTranslationOffset = GetRootToComponent().TransformPosition(LegData.AlignedFootTransformRS.GetLocation()) - LegData.InputPose.FootTransformCS.GetLocation();
            OffsetAverage += LegTranslationOffset / NumLegs;
        }

        // Remove the vertical component
        PelvisOffsetDelta = (OffsetAverage - Context.ApproachDirCS.Dot(OffsetAverage) * Context.ApproachDirCS) * PelvisSettings.HorizontalRebalancingWeight;
        RebalancedPelvisTransform.SetLocation(RebalancedPelvisTransform.GetLocation() + PelvisOffsetDelta);
    }

    // Taken from http://runevision.com/thesis/rune_skovbo_johansen_thesis.pdf
    // Chapter 7.4.2

    float MaxOffsetMin = BIG_NUMBER;
    float DesiredOffsetMin = BIG_NUMBER;
    float DesiredOffsetAvg = 0.0f;
    float MinOffsetMax = -BIG_NUMBER;

    const int32 FootNum = LegsData.Num();
    for (const auto& LegData : LegsData)
    {
        FPelvisOffsetRangeForLimb PelvisOffsetRangeCS;
        FindPelvisOffsetRangeForLimb(
            Context,
            LegData,
            GetRootToComponent().TransformPosition(LegData.AlignedFootTransformRS.GetLocation()),
            RebalancedPelvisTransform,
            PelvisOffsetRangeCS);

        const float DesiredOffset = PelvisOffsetRangeCS.DesiredExtension;
        const float MaxOffset = PelvisOffsetRangeCS.MaxExtension;
        const float MinOffset = PelvisOffsetRangeCS.MinExtension;

        DesiredOffsetAvg += DesiredOffset / FootNum;
        DesiredOffsetMin = FMath::Min(DesiredOffsetMin, DesiredOffset);
        MaxOffsetMin = FMath::Min(MaxOffsetMin, MaxOffset);
        MinOffsetMax = FMath::Max(MinOffsetMax, MinOffset);
    }
    const float MinToAvg = DesiredOffsetAvg - DesiredOffsetMin;
    const float MinToMax = MaxOffsetMin - DesiredOffsetMin;

    DesiredOffsetMin -= 0.05f;

    // In cases like crouching, it favors over-compressing to preserve the pose of the other leg
    // Consider working in over-compression into the formula.
    const float Divisor = MinToAvg + MinToMax;
    float PelvisOffsetZ = FMath::IsNearlyZero(Divisor) ?
        DesiredOffsetMin :
        DesiredOffsetMin + ((MinToAvg * MinToMax) / Divisor);

    // Adjust the hips to prevent over-compression
    PelvisOffsetZ = FMath::Clamp(PelvisOffsetZ, FMath::Min(MinOffsetMax, MaxOffsetMin), MaxOffsetMin);
    PelvisOffsetDelta += -PelvisOffsetZ * Context.ApproachDirCS;

    FTransform PelvisTransformCS = PelvisData.InputPose.FKTransformCS;
    PelvisTransformCS.AddToTranslation(PelvisOffsetDelta);

    return PelvisTransformCS;
}

FTransform FAnimNode_PawniardFootPlacement::UpdatePelvisInterpolationRootSpace(
    const UE::Anim::PawniardFootPlacement::FEvaluationContext& Context,
    const FTransform& TargetPelvisTransformRS)
{
    const auto& RootTransformCS = GetRootToComponent();
    const auto PelvisLocationRS = RootTransformCS.InverseTransformPosition(PelvisData.InputPose.FKTransformCS.GetLocation());

    FTransform OutPelvisTransform = TargetPelvisTransformRS;
    // Calculate the offset from input pose and interpolate
    FVector DesiredPelvisOffset =
        TargetPelvisTransformRS.GetLocation() - PelvisLocationRS;

    // Clamp by MaxOffset
    // Clamping the target before interpolation means we may exceed this purely do to interpolation.
    // If we clamp after, you'll get no smoothing once the limit is reached.
    const float MaxOffsetSqrd = PelvisData.MaxOffsetSqrd;
    const float MaxOffset = PelvisSettings.MaxOffset;
    if (DesiredPelvisOffset.SizeSquared() > MaxOffsetSqrd)
    {
        DesiredPelvisOffset = DesiredPelvisOffset.GetClampedToMaxSize(MaxOffset);
    }

    // Spring interpolation may cause hyperextension/compression so we solve that in FinalizeFootAlignment
    const auto NewTranslationOffset = UKismetMathLibrary::VectorSpringInterp(
        PelvisData.Interpolation.PelvisTranslationOffset, DesiredPelvisOffset, PelvisData.Interpolation.PelvisTranslationSpringState,
        PelvisSettings.LinearStiffness,
        PelvisSettings.LinearDamping,
        Context.UpdateDeltaTime, 1.0f, 0.0f);
    PelvisData.Interpolation.PelvisTranslationOffset = NewTranslationOffset;

    OutPelvisTransform.SetLocation(
        PelvisLocationRS + PelvisData.Interpolation.PelvisTranslationOffset);

    return OutPelvisTransform;
}
