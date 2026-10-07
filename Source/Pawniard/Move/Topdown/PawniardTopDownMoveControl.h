// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavigationTypes.h"
#include "Components/ActorComponent.h"
#include "PawniardTopDownMoveControl.generated.h"

struct FInputActionValue;
class AController;
class APawn;
class INavMovementInterface;
class UEnhancedInputComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UInputMappingContext;
class UPathFollowingComponent;

/** Controller-owned top-down input and navigation, adapted to each movement backend. */
UCLASS(Abstract, ClassGroup=(Pawniard), meta=(ToolTip="Should Be Attached to Controller"))
class PAWNIARD_API UPawniardTopDownMoveControl : public UActorComponent
{
	GENERATED_BODY()

public:
	UPawniardTopDownMoveControl();

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
	FRotator CameraOrientation = FRotator::ZeroRotator;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector NavIntentLocation = FNavigationSystem::InvalidLocation;

	FVector2D AxisIntentVector = FVector2D::ZeroVector;

	virtual void InitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	virtual void BindMoveActions();

	UFUNCTION()
	void HandleOwnerPawnChanged(APawn* OldPawn, APawn* NewPawn);

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	virtual INavMovementInterface* GetNavMovementInterface() const
		PURE_VIRTUAL(UPawniardTopDownMoveControl::GetNavMovementInterface, return nullptr;);

	/** Simulation-driven backends sample the cached intent instead of pushing input in this callback. */
	virtual void ApplyManualMoveInput(const FVector& MoveIntent) {}
	virtual void OnPawnAttached() {}
	virtual void OnPawnDetached() {}
	virtual void ResetNavIntentLocation();

	FVector GetManualMoveIntent() const;
	UPathFollowingComponent* GetNavigationPathFollowingComponent() const;
	void StopNavigation();

private:
	void UnbindMoveActions();
	void HandleAxisInput(const FInputActionValue& Value);
	void HandleAxisInputCompleted();
	void HandleNavInput();
	void UpdateCameraOrientation();
	void BindPathFollowingComponent();
	void DetachFromPawn();

	TWeakObjectPtr<UEnhancedInputComponent> BoundInputComponent;
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;
	TWeakObjectPtr<UInputMappingContext> AddedMappingContext;
	TArray<uint32> InputBindingHandles;
};
