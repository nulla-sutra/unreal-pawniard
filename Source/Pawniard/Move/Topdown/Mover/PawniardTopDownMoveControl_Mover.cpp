// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "PawniardTopDownMoveControl_Mover.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "MoverComponent.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "DefaultMovementSet/NavMoverComponent.h"
#include "Engine/HitResult.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Navigation/PathFollowingComponent.h"
#include "Wynaut/Trace/WynautTraceLibrary.h"

UPawniardTopDownMoveControl_Mover::UPawniardTopDownMoveControl_Mover()
{
    PrimaryComponentTick.bCanEverTick = true;
    bWantsInitializeComponent = true;
}

void UPawniardTopDownMoveControl_Mover::InitializeComponent()
{
    Super::InitializeComponent();
    Controller = Cast<AController>(GetOwner());
    ensureMsgf(Controller, TEXT("Pawniard movement control must be attached to a Controller."));
}

void UPawniardTopDownMoveControl_Mover::BeginPlay()
{
    Super::BeginPlay();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandleOwnerPawnChanged);
        HandleOwnerPawnChanged(nullptr, Controller->GetPawn());
    }
    BindMoveActions();
}

void UPawniardTopDownMoveControl_Mover::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnbindMoveActions();
    DetachFromPawn();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandleOwnerPawnChanged);
    }
    Super::EndPlay(EndPlayReason);
}

void UPawniardTopDownMoveControl_Mover::OnRegister()
{
    Super::OnRegister();
    if (HasBegunPlay() && Controller)
    {
        Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandleOwnerPawnChanged);
        HandleOwnerPawnChanged(nullptr, Controller->GetPawn());
        BindMoveActions();
    }
}

void UPawniardTopDownMoveControl_Mover::OnUnregister()
{
    UnbindMoveActions();
    DetachFromPawn();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandleOwnerPawnChanged);
    }
    Super::OnUnregister();
}

void UPawniardTopDownMoveControl_Mover::TickComponent(const float DeltaTime, const ELevelTick TickType,
                                                     FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    const auto PC = Cast<APlayerController>(Controller);
    if (PC && PC->IsLocalPlayerController())
    {
        CameraOrientation = FRotator(0, PC->PlayerCameraManager
            ? PC->PlayerCameraManager->GetCameraCacheView().Rotation.Yaw
            : PC->GetControlRotation().Yaw, 0);

        // A controller's input component can be created after BeginPlay or replaced on possession.
        if (BoundInputComponent.Get() != Cast<UEnhancedInputComponent>(PC->InputComponent))
        {
            UnbindMoveActions();
            BindMoveActions();
        }
    }
}

void UPawniardTopDownMoveControl_Mover::BindMoveActions()
{
    const auto PC = Cast<APlayerController>(Controller);
    if (!PC || !PC->IsLocalPlayerController() || BoundInputComponent.IsValid())
    {
        return;
    }
    const auto Input = Cast<UEnhancedInputComponent>(PC->InputComponent);
    if (!Input)
    {
        return;
    }
    BoundInputComponent = Input;

    if (AxisInputAction)
    {
        InputBindingHandles.Add(Input->BindAction(AxisInputAction, ETriggerEvent::Triggered,
            this, &ThisClass::HandleAxisInput).GetHandle());
        InputBindingHandles.Add(Input->BindAction(AxisInputAction, ETriggerEvent::Completed,
            this, &ThisClass::HandleAxisInputCompleted).GetHandle());
        InputBindingHandles.Add(Input->BindAction(AxisInputAction, ETriggerEvent::Canceled,
            this, &ThisClass::HandleAxisInputCompleted).GetHandle());
    }
    if (NavInputAction)
    {
        InputBindingHandles.Add(Input->BindAction(NavInputAction, ETriggerEvent::Started,
            this, &ThisClass::ResetNavIntentLocation).GetHandle());
        InputBindingHandles.Add(Input->BindAction(NavInputAction, ETriggerEvent::Triggered,
            this, &ThisClass::HandleNavInput).GetHandle());
        InputBindingHandles.Add(Input->BindAction(NavInputAction, ETriggerEvent::Canceled,
            this, &ThisClass::StopNavigation).GetHandle());
    }

    if (const auto LocalPlayer = PC->GetLocalPlayer())
    {
        const auto Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
        if (Subsystem && MoveMappingContext && !Subsystem->HasMappingContext(MoveMappingContext))
        {
            Subsystem->AddMappingContext(MoveMappingContext, 0);
            BoundInputSubsystem = Subsystem;
            AddedMappingContext = MoveMappingContext.Get();
        }
    }
}

void UPawniardTopDownMoveControl_Mover::UnbindMoveActions()
{
    if (const auto Input = BoundInputComponent.Get())
    {
        for (const auto Handle : InputBindingHandles)
        {
            Input->RemoveBindingByHandle(Handle);
        }
    }
    if (BoundInputSubsystem.IsValid() && AddedMappingContext.IsValid())
    {
        BoundInputSubsystem->RemoveMappingContext(AddedMappingContext.Get());
    }
    InputBindingHandles.Reset();
    BoundInputComponent.Reset();
    BoundInputSubsystem.Reset();
    AddedMappingContext.Reset();
    AxisIntentVector = FVector2D::ZeroVector;
}

void UPawniardTopDownMoveControl_Mover::HandleAxisInput(const FInputActionValue& Value)
{
    AxisIntentVector = Value.Get<FVector2D>();
    if (!AxisIntentVector.IsNearlyZero())
    {
        StopNavigation();
    }
}

void UPawniardTopDownMoveControl_Mover::HandleAxisInputCompleted()
{
    AxisIntentVector = FVector2D::ZeroVector;
}

void UPawniardTopDownMoveControl_Mover::HandleNavInput()
{
    const auto PC = Cast<APlayerController>(Controller);
    if (!PC || !Pawn || !NavMoverComponent.IsValid() || !AxisIntentVector.IsNearlyZero())
    {
        return;
    }
    if (PC->IsMoveInputIgnored())
    {
        StopNavigation();
        return;
    }

    FHitResult TargetHit;
    UWynautTraceLibrary::CursorTraceSingleByChannel(PC, TargetHit, ECC_Camera, {Pawn.Get()});
    if (!TargetHit.bBlockingHit || !TargetHit.GetComponent()
        || TargetHit.GetComponent()->GetCollisionObjectType() != ECC_WorldStatic
        || !FNavigationSystem::IsValidLocation(TargetHit.Location))
    {
        return;
    }
    if (FNavigationSystem::IsValidLocation(NavIntentLocation)
        && TargetHit.Location.Equals(NavIntentLocation, 1.0f))
    {
        return;
    }

    NavIntentLocation = TargetHit.Location;
    if (const auto PathFollower = PC->FindComponentByClass<UPathFollowingComponent>())
    {
        PathFollower->SetNavMovementInterface(NavMoverComponent.Get());
    }
    // Let the engine own the path and follow its segments instead of seeking a path point per input tick.
    UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, NavIntentLocation);
}

void UPawniardTopDownMoveControl_Mover::ProduceInput_Implementation(int32 SimTimeMs,
                                                                   FMoverInputCmdContext& InputCmdResult)
{
    if (!Controller || !Pawn || !Pawn->IsLocallyControlled())
    {
        return;
    }

    auto& Input = InputCmdResult.InputCollection.FindOrAddMutableDataByType<FCharacterDefaultInputs>();
    Input.ControlRotation = CameraOrientation;
    Input.OrientationIntent = FVector::ZeroVector;
    Input.SetMoveInput(EMoveInputType::DirectionalIntent, FVector::ZeroVector);

    if (Controller->IsMoveInputIgnored())
    {
        StopNavigation();
        return;
    }

    if (!AxisIntentVector.IsNearlyZero())
    {
        const auto MoveIntent = CameraOrientation.RotateVector(
            FVector(AxisIntentVector.X, AxisIntentVector.Y, 0)).GetClampedToMaxSize(1.0f);
        Input.SetMoveInput(EMoveInputType::DirectionalIntent, MoveIntent);
        Input.OrientationIntent = MoveIntent.GetSafeNormal();
    }
    else if (const auto NavMover = NavMoverComponent.Get())
    {
        const auto PathFollower = Cast<UPathFollowingComponent>(NavMover->GetPathFollowingAgent());
        if (PathFollower && PathFollower->GetStatus() == EPathFollowingStatus::Moving)
        {
            FVector MoveIntent = FVector::ZeroVector;
            FVector MoveVelocity = FVector::ZeroVector;
            NavMover->ConsumeNavMovementData(MoveIntent, MoveVelocity);
            const auto bUseVelocity = !MoveVelocity.IsNearlyZero();
            const auto MoveInput = bUseVelocity ? MoveVelocity : MoveIntent;
            Input.SetMoveInput(bUseVelocity ? EMoveInputType::Velocity : EMoveInputType::DirectionalIntent, MoveInput);
            Input.OrientationIntent = MoveInput.GetSafeNormal();
        }
    }
}

void UPawniardTopDownMoveControl_Mover::HandleOwnerPawnChanged(APawn* OldPawn, APawn* NewPawn)
{
    DetachFromPawn();
    Pawn = NewPawn;
    if (Pawn)
    {
        MoverComponent = Pawn->FindComponentByClass<UMoverComponent>();
        NavMoverComponent = Pawn->FindComponentByClass<UNavMoverComponent>();
        if (MoverComponent)
        {
            MoverComponent->InputProducer = this;
            // UE 5.8 caches this list at BeginPlay; later possession must update it explicitly.
            MoverComponent->InputProducers.AddUnique(this);
        }
        if (Controller)
        {
            if (const auto PathFollower = Controller->FindComponentByClass<UPathFollowingComponent>())
            {
                PathFollower->SetNavMovementInterface(NavMoverComponent.Get());
            }
        }
    }
}

void UPawniardTopDownMoveControl_Mover::DetachFromPawn()
{
    if (Pawn)
    {
        StopNavigation();
        if (Controller)
        {
            if (const auto PathFollower = Controller->FindComponentByClass<UPathFollowingComponent>())
            {
                PathFollower->SetNavMovementInterface(nullptr);
            }
        }
    }
    if (MoverComponent)
    {
        MoverComponent->InputProducers.Remove(this);
        if (MoverComponent->InputProducer == this)
        {
            MoverComponent->InputProducer = nullptr;
        }
    }
    Pawn = nullptr;
    MoverComponent = nullptr;
    NavMoverComponent.Reset();
    AxisIntentVector = FVector2D::ZeroVector;
    ResetNavIntentLocation();
}

void UPawniardTopDownMoveControl_Mover::StopNavigation()
{
    if (Controller)
    {
        if (const auto PathFollower = Controller->FindComponentByClass<UPathFollowingComponent>())
        {
            if (PathFollower->GetStatus() != EPathFollowingStatus::Idle)
            {
                PathFollower->AbortMove(*this, FPathFollowingResultFlags::OwnerFinished,
                    FAIRequestID::AnyRequest, EPathFollowingVelocityMode::Keep);
            }
        }
    }
    if (const auto NavMover = NavMoverComponent.Get())
    {
        if (const auto PathFollower = Cast<UPathFollowingComponent>(NavMover->GetPathFollowingAgent()))
        {
            if (PathFollower->GetStatus() != EPathFollowingStatus::Idle)
            {
                PathFollower->AbortMove(*this, FPathFollowingResultFlags::OwnerFinished,
                    FAIRequestID::AnyRequest, EPathFollowingVelocityMode::Keep);
            }
        }
    }
    ResetNavIntentLocation();
}

void UPawniardTopDownMoveControl_Mover::ResetNavIntentLocation()
{
    NavIntentLocation = FNavigationSystem::InvalidLocation;
}
