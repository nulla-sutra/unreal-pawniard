// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MoverSimulationTypes.h"
#include "AI/Navigation/NavigationTypes.h"
#include "Components/ActorComponent.h"
#include "PawniardTopDownMoveControl_Mover.generated.h"

struct FInputActionValue;
class AController;
class APawn;
class UEnhancedInputComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UInputMappingContext;
class UMoverComponent;
class UNavMoverComponent;

UCLASS(ClassGroup=(Pawniard), meta=(BlueprintSpawnableComponent, ToolTip="Should Be Attached to Controller"))
class PAWNIARD_API UPawniardTopDownMoveControl_Mover : public UActorComponent, public IMoverInputProducerInterface
{
	GENERATED_BODY()

public:
	UPawniardTopDownMoveControl_Mover();

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input")
	TObjectPtr<UInputAction> AxisInputAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input")
	TObjectPtr<UInputAction> NavInputAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input")
	TObjectPtr<UInputMappingContext> MoveMappingContext;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TObjectPtr<AController> Controller;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TObjectPtr<APawn> Pawn;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TObjectPtr<UMoverComponent> MoverComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FRotator CameraOrientation = FRotator::ZeroRotator;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector NavIntentLocation = FNavigationSystem::InvalidLocation;

	virtual void ProduceInput_Implementation(int32 SimTimeMs, FMoverInputCmdContext& InputCmdResult) override;

	virtual void InitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;


	UFUNCTION()
	void HandleOwnerPawnChanged(APawn* OldPawn, APawn* NewPawn);

	virtual void ResetNavIntentLocation();

private:
	void BindMoveActions();
	void UnbindMoveActions();
	void HandleAxisInput(const FInputActionValue& Value);
	void HandleAxisInputCompleted();
	void HandleNavInput();
	void StopNavigation();
	void DetachFromPawn();

	TWeakObjectPtr<UNavMoverComponent> NavMoverComponent;
	TWeakObjectPtr<UEnhancedInputComponent> BoundInputComponent;
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;
	TWeakObjectPtr<UInputMappingContext> AddedMappingContext;
	TArray<uint32> InputBindingHandles;
	FVector2D AxisIntentVector = FVector2D::ZeroVector;
};
