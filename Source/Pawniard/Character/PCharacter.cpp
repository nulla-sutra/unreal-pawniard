// Copyright 2019-Present tarnishablec. All Rights Reserved.


#include "PCharacter.h"

#include "Components/ArrowComponent.h"
#include "Components/CapsuleComponent.h"
#include "DefaultMovementSet/NavMoverComponent.h"
#include "Engine/CollisionProfile.h"
#include "Navigation/PathFollowingComponent.h"


// Sets default values
APCharacter::APCharacter()
{
    // Set this pawn to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
    PrimaryActorTick.bCanEverTick = true;

    CollisionCapsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CollisionCapsule"));
    ensure(CollisionCapsule);
    SetRootComponent(CollisionCapsule);

    static float InitialCapsuleRadius = 34.0f;
    static float InitialCapsuleHalfHeight = 88.0f;
    CollisionCapsule->InitCapsuleSize(InitialCapsuleRadius, InitialCapsuleHalfHeight);
    CollisionCapsule->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);

    CollisionCapsule->CanCharacterStepUpOn = ECB_No;
    CollisionCapsule->SetShouldUpdatePhysicsVolume(true);
    CollisionCapsule->SetCanEverAffectNavigation(false);
    CollisionCapsule->bDynamicObstacle = true;

    struct FConstructorStatics
    {
        FName ID_Characters;
        FText Name_Characters;

        FConstructorStatics()
            : ID_Characters(TEXT("PCharacters"))
              , Name_Characters(NSLOCTEXT("SpriteCategory", "PCharacters", "PCharacters"))
        {
        }
    };
    static FConstructorStatics ConstructorStatics;

    // Arrow
#if WITH_EDITORONLY_DATA
    ArrowComponent = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
    if (ArrowComponent)
    {
        ArrowComponent->ArrowColor = FColor(150, 200, 255);
        ArrowComponent->bTreatAsASprite = true;
        ArrowComponent->SpriteInfo.Category = ConstructorStatics.ID_Characters;
        ArrowComponent->SpriteInfo.DisplayName = ConstructorStatics.Name_Characters;
        ArrowComponent->SetupAttachment(CollisionCapsule);
        ArrowComponent->bIsScreenSizeScaled = true;
        ArrowComponent->SetSimulatePhysics(false);
    }
#endif // WITH_EDITORONLY_DATA

    // Mover
    MoverComponent = CreateDefaultSubobject<UMoverComponent>(TEXT("MoverComponent"));
    ensure(MoverComponent);

    //
    bReplicates = true;
    bReplicateUsingRegisteredSubObjectList = true;
    SetReplicatingMovement(false);

    // NavMover
    NavMoverComponent = CreateDefaultSubobject<UNavMoverComponent>(TEXT("NavMoverComponent"));
    NavMoverComponent->UpdateNavAgent(*CollisionCapsule);
    NavMoverComponent->SetPathFollowingAgent(PathFollowingComponent);
    PathFollowingComponent = CreateDefaultSubobject<UPathFollowingComponent>(TEXT("PathFollowingComponent"));
}

// Called when the game starts or when spawned
void APCharacter::BeginPlay()
{
    Super::BeginPlay();
    PathFollowingComponent->SetNavMovementInterface(NavMoverComponent);
}

FVector APCharacter::GetNavAgentLocation() const
{
    return NavMoverComponent->GetFeetLocation();
}

IPathFollowingAgentInterface* APCharacter::GetPathFollowingAgent() const
{
    return PathFollowingComponent;
}

const FNavAgentProperties& APCharacter::GetNavAgentPropertiesRef() const
{
    return NavMoverComponent->GetNavAgentPropertiesRef();
}

void APCharacter::PossessedBy(AController* NewController)
{
    Super::PossessedBy(NewController);
}

void APCharacter::OnRep_Controller()
{
    Super::OnRep_Controller();
}
