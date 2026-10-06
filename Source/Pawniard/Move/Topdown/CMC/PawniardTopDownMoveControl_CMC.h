// Copyright 2019-Present tarnishablec. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavigationTypes.h"
#include "Components/ActorComponent.h"
#include "PawniardTopDownMoveControl_CMC.generated.h"

struct FInputActionValue;
class AController;
class APawn;
class UEnhancedInputComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UInputMappingContext;

UCLASS(ClassGroup=(Pawniard), meta=(BlueprintSpawnableComponent, ToolTip="Should Be Attached to Controller"))
class PAWNIARD_API UPawniardTopDownMoveControl_CMC : public UActorComponent
{
    GENERATED_BODY()

public:
    UPawniardTopDownMoveControl_CMC();

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
    TObjectPtr<UInputAction> AxisInputAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
    TObjectPtr<UInputAction> NavInputAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
    TObjectPtr<UInputMappingContext> MoveMappingContext;

    UPROPERTY(BlueprintReadOnly)
    TObjectPtr<AController> Controller;

protected:
    virtual void InitializeComponent() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

    virtual void BindMoveActions();

    UFUNCTION()
    void HandleOwnerPawnChanged(APawn* OldPawn, APawn* NewPawn);

    FVector2D AxisIntentVector = FVector2D::ZeroVector;
    FVector NavIntentLocationCache = FNavigationSystem::InvalidLocation;

private:
    void UnbindMoveActions();
    void HandleAxisInput(const FInputActionValue& Value);
    void HandleAxisInputCompleted();
    void HandleNavInputStarted();
    void HandleNavInput();
    void StopNavigation();

    TWeakObjectPtr<UEnhancedInputComponent> BoundInputComponent;
    TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;
    TWeakObjectPtr<UInputMappingContext> AddedMappingContext;
    TArray<uint32> InputBindingHandles;
};
