// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "PawniardTopDownMoveControl_CMC.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
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

UPawniardTopDownMoveControl_CMC::UPawniardTopDownMoveControl_CMC()
{
    PrimaryComponentTick.bCanEverTick = true;
    bWantsInitializeComponent = true;
}

void UPawniardTopDownMoveControl_CMC::InitializeComponent()
{
    Super::InitializeComponent();
    Controller = Cast<AController>(GetOwner());
    ensureMsgf(Controller, TEXT("Pawniard movement control must be attached to a Controller."));
}

void UPawniardTopDownMoveControl_CMC::BeginPlay()
{
    Super::BeginPlay();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandleOwnerPawnChanged);
        HandleOwnerPawnChanged(nullptr, Controller->GetPawn());
    }
    BindMoveActions();
}

void UPawniardTopDownMoveControl_CMC::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnbindMoveActions();
    StopNavigation();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandleOwnerPawnChanged);
    }
    Super::EndPlay(EndPlayReason);
}

void UPawniardTopDownMoveControl_CMC::OnRegister()
{
    Super::OnRegister();
    if (HasBegunPlay() && Controller)
    {
        Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandleOwnerPawnChanged);
        HandleOwnerPawnChanged(nullptr, Controller->GetPawn());
        BindMoveActions();
    }
}

void UPawniardTopDownMoveControl_CMC::OnUnregister()
{
    UnbindMoveActions();
    StopNavigation();
    if (Controller)
    {
        Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandleOwnerPawnChanged);
    }
    Super::OnUnregister();
}

void UPawniardTopDownMoveControl_CMC::TickComponent(const float DeltaTime, const ELevelTick TickType,
                                                   FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    const auto PC = Cast<APlayerController>(Controller);
    if (PC && PC->IsLocalPlayerController()
        && BoundInputComponent.Get() != Cast<UEnhancedInputComponent>(PC->InputComponent))
    {
        // Input components may appear after BeginPlay or be replaced when the controller changes pawn.
        UnbindMoveActions();
        BindMoveActions();
    }
}

void UPawniardTopDownMoveControl_CMC::BindMoveActions()
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
            this, &ThisClass::HandleNavInputStarted).GetHandle());
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

void UPawniardTopDownMoveControl_CMC::UnbindMoveActions()
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

void UPawniardTopDownMoveControl_CMC::HandleAxisInput(const FInputActionValue& Value)
{
    const auto PC = Cast<APlayerController>(Controller);
    if (!PC || !PC->GetPawn())
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
        const auto CameraYaw = PC->PlayerCameraManager
            ? PC->PlayerCameraManager->GetCameraCacheView().Rotation.Yaw
            : PC->GetControlRotation().Yaw;
        const auto MoveIntent = FRotator(0, CameraYaw, 0).RotateVector(
            FVector(AxisIntentVector.X, AxisIntentVector.Y, 0)).GetClampedToMaxSize(1.0f);
        PC->GetPawn()->AddMovementInput(MoveIntent);
    }
}

void UPawniardTopDownMoveControl_CMC::HandleAxisInputCompleted()
{
    AxisIntentVector = FVector2D::ZeroVector;
}

void UPawniardTopDownMoveControl_CMC::HandleNavInput()
{
    const auto PC = Cast<APlayerController>(Controller);
    if (!PC || !PC->GetPawn() || !AxisIntentVector.IsNearlyZero())
    {
        return;
    }
    if (PC->IsMoveInputIgnored())
    {
        StopNavigation();
        return;
    }

    FHitResult TargetHit;
    UWynautTraceLibrary::CursorTraceSingleByChannel(PC, TargetHit, ECC_Camera, {PC->GetPawn()});
    if (!TargetHit.bBlockingHit || !TargetHit.GetComponent()
        || TargetHit.GetComponent()->GetCollisionObjectType() != ECC_WorldStatic
        || !FNavigationSystem::IsValidLocation(TargetHit.Location))
    {
        return;
    }
    if (FNavigationSystem::IsValidLocation(NavIntentLocationCache)
        && TargetHit.Location.Equals(NavIntentLocationCache, 1.0f))
    {
        return;
    }
    NavIntentLocationCache = TargetHit.Location;
    UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, NavIntentLocationCache);
}

void UPawniardTopDownMoveControl_CMC::HandleNavInputStarted()
{
    NavIntentLocationCache = FNavigationSystem::InvalidLocation;
}

void UPawniardTopDownMoveControl_CMC::HandleOwnerPawnChanged(APawn* OldPawn, APawn* NewPawn)
{
    StopNavigation();
    AxisIntentVector = FVector2D::ZeroVector;
    if (Controller)
    {
        if (const auto PathFollower = Controller->FindComponentByClass<UPathFollowingComponent>())
        {
            PathFollower->SetNavMovementInterface(NewPawn
                ? NewPawn->FindComponentByInterface<INavMovementInterface>() : nullptr);
        }
    }
}

void UPawniardTopDownMoveControl_CMC::StopNavigation()
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
    NavIntentLocationCache = FNavigationSystem::InvalidLocation;
}
