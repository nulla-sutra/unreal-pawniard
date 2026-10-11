// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "PawniardPoseSearchSlopeChannel.h"

#include "Animation/AnimInstance.h"
#include "Animation/TrajectoryTypes.h"
#include "CoreGlobals.h"
#include "IObjectChooser.h"
#include "Misc/ScopeLock.h"
#include "PoseSearch/PoseSearchContext.h"
#include "PoseSearch/PoseSearchHistory.h"
#include "PoseSearch/PoseSearchTrajectoryLibrary.h"
#if WITH_EDITOR
#include "UObject/UnrealType.h"
#endif

namespace UE::Anim::PawniardPoseSearch
{
    static float GetRampWeight(double Value, float Minimum, float FullWeight)
    {
        if (FullWeight <= Minimum)
        {
            return Value > Minimum ? 1.0f : 0.0f;
        }

        return static_cast<float>(FMath::Clamp((Value - Minimum) / (FullWeight - Minimum), 0.0, 1.0));
    }
}

UPawniardPoseSearchSlopeChannel::UPawniardPoseSearchSlopeChannel()
{
    CurveName = TEXT("LocomotionSlope");
    InputQueryPose = EInputQueryPose::UseCharacterPose;
#if WITH_EDITORONLY_DATA
    Weight = 10.0f;
#endif
}

void UPawniardPoseSearchSlopeChannel::BuildQuery(UE::PoseSearch::FSearchContext& SearchContext) const
{
    auto Slope = 0.0f;
    const auto* Context = SearchContext.GetContext(SampleRole);
    const auto* AnimInstance = Context ? Cast<UAnimInstance>(Context->GetFirstObjectParam()) : nullptr;
    const auto* PoseHistory = SearchContext.GetPoseHistory(SampleRole);
    if (AnimInstance && PoseHistory)
    {
        Slope = SampleInstanceSlope(*AnimInstance, *PoseHistory);
    }

    UE::PoseSearch::FFeatureVectorHelper::EncodeFloat(SearchContext.EditFeatureVector(), ChannelDataOffset, Slope);
}

float UPawniardPoseSearchSlopeChannel::SampleInstanceSlope(
    const UAnimInstance& AnimInstance, const UE::PoseSearch::IPoseHistory& PoseHistory) const
{
    const auto& NativeTrajectory = PoseHistory.GetTrajectory();
    const auto DeltaTime = AnimInstance.GetDeltaSeconds();
    if (NativeTrajectory.Samples.Num() < 2 || !FMath::IsFinite(DeltaTime) || DeltaTime <= UE_SMALL_NUMBER
        || !FMath::IsFinite(SamplingWindow) || SamplingWindow <= UE_SMALL_NUMBER
        || !FMath::IsFinite(SampleTimeOffset)
        || SampleTimeOffset < NativeTrajectory.Samples[0].TimeInSeconds
        || SampleTimeOffset > NativeTrajectory.Samples.Last().TimeInSeconds)
    {
        return 0.0f;
    }

    const auto Sample = NativeTrajectory.GetSampleAtTime(SampleTimeOffset, false);
    if (Sample.Position.ContainsNaN() || Sample.Facing.ContainsNaN())
    {
        return 0.0f;
    }

    FScopeLock Lock(&HistoriesMutex);
    const auto Frame = GFrameCounter;
    if (Frame - LastCleanupFrame >= 64)
    {
        for (auto It = InstanceHistories.CreateIterator(); It; ++It)
        {
            if (!It.Key().IsValid(false, true))
            {
                It.RemoveCurrent();
            }
        }
        LastCleanupFrame = Frame;
    }

    auto& History = InstanceHistories.FindOrAdd(TWeakObjectPtr<const UAnimInstance>(&AnimInstance));
    constexpr int32 HistorySampleCount = 6;
    FPoseSearchTrajectoryData::FSampling Sampling;
    Sampling.NumHistorySamples = HistorySampleCount;
    Sampling.NumPredictionSamples = 0;
    Sampling.SecondsPerHistorySample = SamplingWindow / 4.0f;
    Sampling.SecondsPerPredictionSample = 0.0f;

    const auto bSettingsChanged = History.Source != &PoseHistory
        || History.SamplingWindow != SamplingWindow || History.SampleTimeOffset != SampleTimeOffset;
    if (History.LastFrame != Frame || bSettingsChanged)
    {
        const auto bHasHistory = History.Snapshots.Samples.Num() == HistorySampleCount + 1;
        const auto bPositionJump = bHasHistory && HistoryResetDistance > 0.0f
            && FVector::DistSquared(Sample.Position, History.Snapshots.Samples.Last().Position)
                > FMath::Square(static_cast<double>(HistoryResetDistance));
        if (!bHasHistory || bSettingsChanged || History.LastFrame + 1 != Frame || bPositionJump)
        {
            History = FInstanceHistory();
            UPoseSearchTrajectoryLibrary::InitTrajectorySamples(
                History.Snapshots, Sample.Position, Sample.Facing, Sampling, DeltaTime);
        }
        else
        {
            // Native movement-relative history rewrites old heights. Keep only its sampled
            // position per update, and let the engine's world-space history helper age it.
            UPoseSearchTrajectoryLibrary::UpdateHistory_WorldSpace(History.Snapshots, Sampling, DeltaTime);
            History.AvailableSeconds = FMath::Min(History.AvailableSeconds + DeltaTime, SamplingWindow);
        }

        auto& CurrentSample = History.Snapshots.Samples[HistorySampleCount];
        CurrentSample = Sample;
        CurrentSample.TimeInSeconds = DeltaTime;
        History.LastFrame = Frame;
        History.Source = &PoseHistory;
        History.SamplingWindow = SamplingWindow;
        History.SampleTimeOffset = SampleTimeOffset;
    }

    // A repeated search in one frame must not advance time or duplicate its position.
    // Initialized history slots are placeholders until a real sampling window has elapsed.
    return History.AvailableSeconds >= SamplingWindow
        ? CalculateSlope(History.Snapshots, History.Snapshots.Samples.Last().TimeInSeconds)
        : 0.0f;
}

float UPawniardPoseSearchSlopeChannel::CalculateSlope(const FTransformTrajectory& Trajectory, float EndTime) const
{
    if (Trajectory.Samples.Num() < 2 || !FMath::IsFinite(EndTime)
        || !FMath::IsFinite(SamplingWindow) || SamplingWindow <= UE_SMALL_NUMBER
        || !FMath::IsFinite(MinHorizontalSpeed) || !FMath::IsFinite(FullWeightHorizontalSpeed)
        || UpDirectionWS.ContainsNaN())
    {
        return 0.0f;
    }

    const auto StartTime = EndTime - SamplingWindow;
    const auto FirstTime = Trajectory.Samples[0].TimeInSeconds;
    const auto LastTime = Trajectory.Samples.Last().TimeInSeconds;
    if (!FMath::IsFinite(StartTime) || !FMath::IsFinite(FirstTime) || !FMath::IsFinite(LastTime)
        || StartTime < FirstTime || EndTime > LastTime)
    {
        // A clamped or extrapolated endpoint would fabricate a slope during startup
        // or after a trajectory reset. Wait until the requested window is available.
        return 0.0f;
    }

    const auto UpWS = UpDirectionWS.GetSafeNormal();
    if (UpWS.IsNearlyZero())
    {
        return 0.0f;
    }

    const auto StartSample = Trajectory.GetSampleAtTime(StartTime, false);
    const auto EndSample = Trajectory.GetSampleAtTime(EndTime, false);
    if (StartSample.Position.ContainsNaN() || EndSample.Position.ContainsNaN())
    {
        return 0.0f;
    }

    const auto DeltaWS = EndSample.Position - StartSample.Position;
    const auto HeightDelta = FVector::DotProduct(DeltaWS, UpWS);
    const auto HorizontalDelta = DeltaWS - UpWS * HeightDelta;
    const auto HorizontalDistance = HorizontalDelta.Size();
    if (!FMath::IsFinite(HorizontalDistance) || !FMath::IsFinite(HeightDelta)
        || HorizontalDistance <= UE_DOUBLE_SMALL_NUMBER)
    {
        return 0.0f;
    }

    const auto HorizontalSpeed = HorizontalDistance / SamplingWindow;
    auto QueryWeight = UE::Anim::PawniardPoseSearch::GetRampWeight(
        HorizontalSpeed, MinHorizontalSpeed, FullWeightHorizontalSpeed);
    if (bForwardOnly && QueryWeight > 0.0f)
    {
        if (EndSample.Facing.ContainsNaN() || !FMath::IsFinite(MinForwardDot)
            || !FMath::IsFinite(FullWeightForwardDot))
        {
            return 0.0f;
        }

        const auto ForwardWS = FVector::VectorPlaneProject(EndSample.Facing.GetForwardVector(), UpWS).GetSafeNormal();
        const auto ForwardDot = FVector::DotProduct(HorizontalDelta / HorizontalDistance, ForwardWS);
        QueryWeight *= UE::Anim::PawniardPoseSearch::GetRampWeight(ForwardDot, MinForwardDot, FullWeightForwardDot);
    }

    const auto SlopeDegrees = FMath::RadiansToDegrees(FMath::Atan2(HeightDelta, HorizontalDistance));
    return static_cast<float>(SlopeDegrees) * QueryWeight;
}

#if WITH_EDITOR
bool UPawniardPoseSearchSlopeChannel::CanEditChange(const FProperty* InProperty) const
{
    // Reusing the continuing animation's curve would feed the selected clip back
    // into its own slope query instead of measuring the character's trajectory.
    if (InProperty && InProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UPoseSearchFeatureChannel_Curve, InputQueryPose))
    {
        return false;
    }

    return Super::CanEditChange(InProperty);
}
#endif
