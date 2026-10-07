// Copyright 2019-Present tarnishablec. All Rights Reserved.

#include "PawniardTopDownMoveControl_CMC.h"

#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"

UPawniardTopDownMoveControl_CMC::UPawniardTopDownMoveControl_CMC() = default;

INavMovementInterface* UPawniardTopDownMoveControl_CMC::GetNavMovementInterface() const
{
    return Pawn ? Pawn->GetMovementComponent() : nullptr;
}

void UPawniardTopDownMoveControl_CMC::ApplyManualMoveInput(const FVector& MoveIntent)
{
    if (Pawn)
    {
        Pawn->AddMovementInput(MoveIntent);
    }
}
