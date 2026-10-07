// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "PawniardTopDownMoveControl_Mover.h"

#include "MoverComponent.h"
#include "DefaultMovementSet/NavMoverComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Navigation/PathFollowingComponent.h"

UPawniardTopDownMoveControl_Mover::UPawniardTopDownMoveControl_Mover() = default;

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
        const auto MoveIntent = GetManualMoveIntent();
        Input.SetMoveInput(EMoveInputType::DirectionalIntent, MoveIntent);
        Input.OrientationIntent = MoveIntent.GetSafeNormal();
    }
    else if (const auto NavMover = NavMoverComponent.Get())
    {
        const auto PathFollower = GetNavigationPathFollowingComponent();
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

INavMovementInterface* UPawniardTopDownMoveControl_Mover::GetNavMovementInterface() const
{
    return NavMoverComponent.Get();
}

void UPawniardTopDownMoveControl_Mover::OnPawnAttached()
{
    MoverComponent = Pawn->FindComponentByClass<UMoverComponent>();
    NavMoverComponent = Pawn->FindComponentByClass<UNavMoverComponent>();
    if (MoverComponent)
    {
        MoverComponent->InputProducer = this;
        // UE 5.8 caches this list at BeginPlay; later possession must update it explicitly.
        MoverComponent->InputProducers.AddUnique(this);
    }
}

void UPawniardTopDownMoveControl_Mover::OnPawnDetached()
{
    if (MoverComponent)
    {
        MoverComponent->InputProducers.Remove(this);
        if (MoverComponent->InputProducer == this)
        {
            MoverComponent->InputProducer = nullptr;
        }
    }
    MoverComponent = nullptr;
    NavMoverComponent.Reset();
}
