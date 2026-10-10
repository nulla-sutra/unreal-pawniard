// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "AnimGraphNode_PawniardFootPlacement.h"

#include "Animation/Skeleton.h"
#include "Kismet2/CompilerResultsLog.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AnimGraphNode_PawniardFootPlacement)

#define LOCTEXT_NAMESPACE "PawniardFootPlacement"

UAnimGraphNode_PawniardFootPlacement::UAnimGraphNode_PawniardFootPlacement(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
}

FText UAnimGraphNode_PawniardFootPlacement::GetControllerDescription() const
{
    return LOCTEXT("Title", "Foot Placement (Pawniard)");
}

FText UAnimGraphNode_PawniardFootPlacement::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
    return GetControllerDescription();
}

FText UAnimGraphNode_PawniardFootPlacement::GetTooltipText() const
{
    return LOCTEXT("Tooltip", "Places IK foot targets and the pelvis using scene collision and automatic foot speed. Configure the bones and connect a Leg IK node after this node. No movement component is required.");
}

FLinearColor UAnimGraphNode_PawniardFootPlacement::GetNodeTitleColor() const
{
    return FLinearColor(FColor(153, 0, 153));
}

FString UAnimGraphNode_PawniardFootPlacement::GetNodeCategory() const
{
    return TEXT("Pawniard|Animation");
}

void UAnimGraphNode_PawniardFootPlacement::ValidateAnimNodeDuringCompilation(
    USkeleton* ForSkeleton, FCompilerResultsLog& MessageLog)
{
    Super::ValidateAnimNodeDuringCompilation(ForSkeleton, MessageLog);
    if (!ForSkeleton)
    {
        return;
    }
    const auto& Skeleton = ForSkeleton->GetReferenceSkeleton();
    const auto ValidateBone = [&](const FBoneReference& Bone)
    {
        if (Bone.BoneName.IsNone() || Skeleton.FindBoneIndex(Bone.BoneName) == INDEX_NONE)
        {
            MessageLog.Error(TEXT("@@ references an unset or missing bone '%s'."), this, *Bone.BoneName.ToString());
        }
    };
    ValidateBone(Node.PelvisBone);
    ValidateBone(Node.IKFootRootBone);
    if (Node.LegDefinitions.IsEmpty())
    {
        MessageLog.Error(TEXT("@@ needs at least one leg definition."), this);
    }
    if (Node.PelvisBone.BoneName == Node.IKFootRootBone.BoneName)
    {
        MessageLog.Error(TEXT("@@ requires distinct pelvis and IK foot root bones."), this);
    }
    TSet<FName> OutputBones;
    OutputBones.Add(Node.PelvisBone.BoneName);
    if (Node.InterpolationSettings.bSmoothRootBone && Skeleton.GetNum() > 0)
    {
        const auto RootName = Skeleton.GetBoneName(0);
        if (RootName == Node.PelvisBone.BoneName)
        {
            MessageLog.Error(TEXT("@@ cannot smooth the root when the pelvis is also the root."), this);
        }
        OutputBones.Add(RootName);
    }
    for (const auto& Leg : Node.LegDefinitions)
    {
        ValidateBone(Leg.FKFootBone);
        ValidateBone(Leg.IKFootBone);
        ValidateBone(Leg.BallBone);
        if (Leg.FKFootBone.BoneName == Leg.IKFootBone.BoneName
            || OutputBones.Contains(Leg.IKFootBone.BoneName))
        {
            MessageLog.Error(TEXT("@@ requires distinct FK feet, IK feet and pelvis output bones."), this);
        }
        OutputBones.Add(Leg.IKFootBone.BoneName);
        auto ParentIndex = Skeleton.FindBoneIndex(Leg.FKFootBone.BoneName);
        for (auto Index = 0; Index < Leg.NumBonesInLimb && ParentIndex != INDEX_NONE; ++Index)
        {
            ParentIndex = Skeleton.GetParentIndex(ParentIndex);
        }
        if (Leg.NumBonesInLimb < 1 || ParentIndex == INDEX_NONE)
        {
            MessageLog.Error(TEXT("@@ has an incomplete leg chain for '%s'."), this, *Leg.FKFootBone.BoneName.ToString());
        }
    }
}

#undef LOCTEXT_NAMESPACE
