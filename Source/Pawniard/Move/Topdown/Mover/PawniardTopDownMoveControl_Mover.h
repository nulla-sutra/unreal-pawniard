// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "Pawniard/Move/Topdown/PawniardTopDownMoveControl.h"
#include "MoverSimulationTypes.h"
#include "PawniardTopDownMoveControl_Mover.generated.h"

class UMoverComponent;
class UNavMoverComponent;

UCLASS(ClassGroup=(Pawniard), meta=(BlueprintSpawnableComponent, ToolTip="Should Be Attached to Controller"))
class PAWNIARD_API UPawniardTopDownMoveControl_Mover
	: public UPawniardTopDownMoveControl, public IMoverInputProducerInterface
{
	GENERATED_BODY()

public:
	UPawniardTopDownMoveControl_Mover();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TObjectPtr<UMoverComponent> MoverComponent;

	virtual void ProduceInput_Implementation(int32 SimTimeMs, FMoverInputCmdContext& InputCmdResult) override;

protected:
	virtual INavMovementInterface* GetNavMovementInterface() const override;
	virtual void OnPawnAttached() override;
	virtual void OnPawnDetached() override;

private:
	TWeakObjectPtr<UNavMoverComponent> NavMoverComponent;
};
