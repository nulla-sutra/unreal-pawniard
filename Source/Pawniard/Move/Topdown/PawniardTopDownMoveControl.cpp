// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "PawniardTopDownMoveControl.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "AI/Navigation/PathFollowingAgentInterface.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/HitResult.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/NavMovementInterface.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Navigation/PathFollowingComponent.h"
#include "Wynaut/Trace/WynautTraceLibrary.h"

UPawniardTopDownMoveControl::UPawniardTopDownMoveControl()
{
    PrimaryComponentTick.bCanEverTick = true;
    bWantsInitializeComponent = true;
}

void UPawniardTopDownMoveControl::InitializeComponent()
{
    Super::InitializeComponent();
    Controller = Cast<AController>(GetOwner());
    ensureMsgf(Controller, TEXT("Pawniard movement control must be attached to a Controller."));
}

void UPawniardTopDownMoveControl::BeginPlay()
{
    Super::BeginPlay();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandleOwnerPawnChanged);
        HandleOwnerPawnChanged(nullptr, Controller->GetPawn());
    }
    BindMoveActions();
}

void UPawniardTopDownMoveControl::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnbindMoveActions();
    DetachFromPawn();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandleOwnerPawnChanged);
    }
    Super::EndPlay(EndPlayReason);
}

void UPawniardTopDownMoveControl::OnRegister()
{
    Super::OnRegister();
    if (HasBegunPlay() && Controller)
    {
        Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandleOwnerPawnChanged);
        HandleOwnerPawnChanged(nullptr, Controller->GetPawn());
        BindMoveActions();
    }
}

void UPawniardTopDownMoveControl::OnUnregister()
{
    UnbindMoveActions();
    DetachFromPawn();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandleOwnerPawnChanged);
    }
    Super::OnUnregister();
}

void UPawniardTopDownMoveControl::TickComponent(const float DeltaTime, const ELevelTick TickType,
                                               FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    const auto PC = Cast<APlayerController>(Controller);
    if (PC && PC->IsLocalPlayerController())
    {
        UpdateCameraOrientation();
        if (BoundInputComponent.Get() != Cast<UEnhancedInputComponent>(PC->InputComponent))
        {
            // Input components may appear after BeginPlay or be replaced on possession.
            UnbindMoveActions();
            BindMoveActions();
        }
    }
}

void UPawniardTopDownMoveControl::BindMoveActions()
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

void UPawniardTopDownMoveControl::UnbindMoveActions()
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

void UPawniardTopDownMoveControl::HandleAxisInput(const FInputActionValue& Value)
{
    const auto PC = Cast<APlayerController>(Controller);
    if (!PC || !Pawn)
    {
        return;
    }
    AxisIntentVector = Value.Get<FVector2D>();
    if (PC->IsMoveInputIgnored())
    {
        AxisIntentVector = FVector2D::ZeroVector;
        StopNavigation();
        return;
    }
    if (!AxisIntentVector.IsNearlyZero())
    {
        StopNavigation();
        UpdateCameraOrientation();
        ApplyManualMoveInput(GetManualMoveIntent());
    }
}

void UPawniardTopDownMoveControl::HandleAxisInputCompleted()
{
    AxisIntentVector = FVector2D::ZeroVector;
}

void UPawniardTopDownMoveControl::UpdateCameraOrientation()
{
    if (const auto PC = Cast<APlayerController>(Controller))
    {
        CameraOrientation = FRotator(0, PC->PlayerCameraManager
            ? PC->PlayerCameraManager->GetCameraCacheView().Rotation.Yaw
            : PC->GetControlRotation().Yaw, 0);
    }
}

FVector UPawniardTopDownMoveControl::GetManualMoveIntent() const
{
    return CameraOrientation.RotateVector(
        FVector(AxisIntentVector.X, AxisIntentVector.Y, 0)).GetClampedToMaxSize(1.0f);
}

void UPawniardTopDownMoveControl::HandleNavInput()
{
    const auto PC = Cast<APlayerController>(Controller);
    if (!PC || !Pawn || !GetNavMovementInterface() || !AxisIntentVector.IsNearlyZero())
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
    BindPathFollowingComponent();
    // The engine owns the path and advances its segments for either movement backend.
    UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, NavIntentLocation);
}

void UPawniardTopDownMoveControl::HandleOwnerPawnChanged(APawn* OldPawn, APawn* NewPawn)
{
    DetachFromPawn();
    Pawn = NewPawn;
    if (Pawn)
    {
        OnPawnAttached();
        BindPathFollowingComponent();
    }
}

void UPawniardTopDownMoveControl::BindPathFollowingComponent()
{
    if (Controller)
    {
        if (const auto PathFollower = Controller->FindComponentByClass<UPathFollowingComponent>())
        {
            PathFollower->SetNavMovementInterface(GetNavMovementInterface());
        }
    }
}

UPathFollowingComponent* UPawniardTopDownMoveControl::GetNavigationPathFollowingComponent() const
{
    if (const auto MovementInterface = GetNavMovementInterface())
    {
        return Cast<UPathFollowingComponent>(MovementInterface->GetPathFollowingAgent());
    }
    return nullptr;
}

void UPawniardTopDownMoveControl::DetachFromPawn()
{
    StopNavigation();
    if (Pawn)
    {
        if (Controller)
        {
            if (const auto PathFollower = Controller->FindComponentByClass<UPathFollowingComponent>())
            {
                PathFollower->SetNavMovementInterface(nullptr);
            }
        }
        OnPawnDetached();
    }
    Pawn = nullptr;
    AxisIntentVector = FVector2D::ZeroVector;
}

void UPawniardTopDownMoveControl::StopNavigation()
{
    const auto ControllerPathFollower = Controller
        ? Controller->FindComponentByClass<UPathFollowingComponent>() : nullptr;
    const auto NavigationPathFollower = GetNavigationPathFollowingComponent();
    const auto AbortPath = [this](UPathFollowingComponent* PathFollower)
    {
        if (PathFollower && PathFollower->GetStatus() != EPathFollowingStatus::Idle)
        {
            PathFollower->AbortMove(*this, FPathFollowingResultFlags::OwnerFinished,
                FAIRequestID::AnyRequest, EPathFollowingVelocityMode::Keep);
        }
    };
    AbortPath(ControllerPathFollower);
    // Navigation can also use a pawn-owned follower; abort each distinct agent once.
    if (NavigationPathFollower != ControllerPathFollower)
    {
        AbortPath(NavigationPathFollower);
    }
    ResetNavIntentLocation();
}

void UPawniardTopDownMoveControl::ResetNavIntentLocation()
{
    NavIntentLocation = FNavigationSystem::InvalidLocation;
}
