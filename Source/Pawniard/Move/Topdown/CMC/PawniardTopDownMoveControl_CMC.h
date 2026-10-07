// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "Pawniard/Move/Topdown/PawniardTopDownMoveControl.h"
#include "PawniardTopDownMoveControl_CMC.generated.h"

UCLASS(ClassGroup=(Pawniard), meta=(BlueprintSpawnableComponent, ToolTip="Should Be Attached to Controller"))
class PAWNIARD_API UPawniardTopDownMoveControl_CMC : public UPawniardTopDownMoveControl
{
	GENERATED_BODY()

public:
	UPawniardTopDownMoveControl_CMC();

protected:
	virtual INavMovementInterface* GetNavMovementInterface() const override;
	virtual void ApplyManualMoveInput(const FVector& MoveIntent) override;
};
