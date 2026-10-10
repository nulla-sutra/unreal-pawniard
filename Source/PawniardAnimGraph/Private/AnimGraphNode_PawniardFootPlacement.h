// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "AnimGraphNode_SkeletalControlBase.h"
#include "Pawniard/Animation/AnimNode_PawniardFootPlacement.h"
#include "AnimGraphNode_PawniardFootPlacement.generated.h"

UCLASS()
class PAWNIARDANIMGRAPH_API UAnimGraphNode_PawniardFootPlacement : public UAnimGraphNode_SkeletalControlBase
{
    GENERATED_BODY()

public:
    UAnimGraphNode_PawniardFootPlacement(const FObjectInitializer& ObjectInitializer);

    UPROPERTY(EditAnywhere, Category = "Settings")
    FAnimNode_PawniardFootPlacement Node;

    virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
    virtual FText GetTooltipText() const override;
    virtual FLinearColor GetNodeTitleColor() const override;
    virtual FString GetNodeCategory() const override;
    virtual void ValidateAnimNodeDuringCompilation(USkeleton* ForSkeleton, FCompilerResultsLog& MessageLog) override;

protected:
    virtual FText GetControllerDescription() const override;
    virtual const FAnimNode_SkeletalControlBase* GetNode() const override { return &Node; }
};
