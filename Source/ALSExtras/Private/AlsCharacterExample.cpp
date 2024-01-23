#include "AlsCharacterExample.h"

#include "AlsCameraComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AlsCharacterExample)

AAlsCharacterExample::AAlsCharacterExample()
{
	Camera = CreateDefaultSubobject<UAlsCameraComponent>(FName{TEXTVIEW("Camera")});
	Camera->SetupAttachment(GetMesh());
	Camera->SetRelativeRotation_Direct({0.0f, 90.0f, 0.0f});
}

void AAlsCharacterExample::NotifyControllerChanged()
{
	const auto* PreviousPlayer{Cast<APlayerController>(PreviousController)};
	if (IsValid(PreviousPlayer))
	{
		auto* InputSubsystem{ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PreviousPlayer->GetLocalPlayer())};
		if (IsValid(InputSubsystem))
		{
			InputSubsystem->RemoveMappingContext(InputMappingContext);
		}
	}

	auto* NewPlayer{Cast<APlayerController>(GetController())};
	if (IsValid(NewPlayer))
	{
		NewPlayer->InputYawScale_DEPRECATED = 1.0f;
		NewPlayer->InputPitchScale_DEPRECATED = 1.0f;
		NewPlayer->InputRollScale_DEPRECATED = 1.0f;

		auto* InputSubsystem{ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(NewPlayer->GetLocalPlayer())};
		if (IsValid(InputSubsystem))
		{
			FModifyContextOptions Options;
			Options.bNotifyUserSettings = true;

			InputSubsystem->AddMappingContext(InputMappingContext, 0, Options);
		}
	}

	Super::NotifyControllerChanged();
}

void AAlsCharacterExample::CalcCamera(const float DeltaTime, FMinimalViewInfo& ViewInfo)
{
	if (Camera->IsActive())
	{
		Camera->GetViewInfo(ViewInfo);
		return;
	}

	Super::CalcCamera(DeltaTime, ViewInfo);
}

void AAlsCharacterExample::SetupPlayerInputComponent(UInputComponent* Input)
{
	Super::SetupPlayerInputComponent(Input);

	auto* EnhancedInput{Cast<UEnhancedInputComponent>(Input)};
	if (IsValid(EnhancedInput))
	{
		//Move All Input to Blueprint so I can expend it with other system
		//EnhancedInput->BindAction(LookMouseAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnLookMouse);
		//EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnLook);
		//EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnMove);
		//EnhancedInput->BindAction(SprintAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnSprint);
		//EnhancedInput->BindAction(WalkAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnWalk);
		//EnhancedInput->BindAction(CrouchAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnCrouch);
		//EnhancedInput->BindAction(JumpAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnJump);
// 		EnhancedInput->BindAction(AimAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnAim);
// 		EnhancedInput->BindAction(RagdollAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnRagdoll);
// 		EnhancedInput->BindAction(RollAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnRoll);
// 		EnhancedInput->BindAction(RotationModeAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnRotationMode);
// 		EnhancedInput->BindAction(ViewModeAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnViewMode);
// 		EnhancedInput->BindAction(SwitchShoulderAction, ETriggerEvent::Triggered, this, &ThisClass::Input_OnSwitchShoulder);
	}
}

//void AAlsCharacterExample::Input_OnLookMouse(const FInputActionValue& ActionValue)
void AAlsCharacterExample::Input_OnLookMouse(const FVector2D& Value)
{
	//const auto Value{ActionValue.Get<FVector2D>()};

	AddControllerPitchInput(Value.Y * LookUpMouseSensitivity);
	AddControllerYawInput(Value.X * LookRightMouseSensitivity);
}

void AAlsCharacterExample::Input_OnLook(const FVector2D& Value)
{
	//const auto Value{ActionValue.Get<FVector2D>()};

	AddControllerPitchInput(Value.Y * LookUpRate);
	AddControllerYawInput(Value.X * LookRightRate);
}

void AAlsCharacterExample::Input_OnMove(const FVector2D& Value)
{
	const auto ClampedValue{UAlsMath::ClampMagnitude012D(Value)};

	const auto ForwardDirection{UAlsMath::AngleToDirectionXY(UE_REAL_TO_FLOAT(GetViewState().Rotation.Yaw))};
	const auto RightDirection{UAlsMath::PerpendicularCounterClockwiseXY(ForwardDirection)};

	AddMovementInput(ForwardDirection * ClampedValue.Y + RightDirection * ClampedValue.X);
}

void AAlsCharacterExample::Input_OnSprint(bool Value)
{
	SetDesiredGait(Value ? AlsGaitTags::Sprinting : AlsGaitTags::Running);
}

void AAlsCharacterExample::Input_OnWalk()
{
	if (GetDesiredGait() == AlsGaitTags::Walking)
	{
		SetDesiredGait(AlsGaitTags::Running);
	}
	else if (GetDesiredGait() == AlsGaitTags::Running)
	{
		SetDesiredGait(AlsGaitTags::Walking);
	}
}

void AAlsCharacterExample::Input_OnCrouch()
{
	if (GetDesiredStance() == AlsStanceTags::Standing)
	{
		SetDesiredStance(AlsStanceTags::Crouching);
	}
	else if (GetDesiredStance() == AlsStanceTags::Crouching)
	{
		SetDesiredStance(AlsStanceTags::Standing);
	}
}

void AAlsCharacterExample::Input_OnJump(bool Value)
{
	if (Value)
	{
		if (StopRagdolling())
		{
			return;
		}

		if (StartMantlingGrounded())
		{
			return;
		}

		if (GetStance() == AlsStanceTags::Crouching)
		{
			SetDesiredStance(AlsStanceTags::Standing);
			return;
		}

		Jump();
	}
	else
	{
		StopJumping();
	}
}

void AAlsCharacterExample::Input_OnAim(bool Value)
{
	SetDesiredAiming(Value);
}

void AAlsCharacterExample::Input_OnRagdoll()
{
	if (!StopRagdolling())
	{
		StartRagdolling();
	}
}

void AAlsCharacterExample::Input_OnRoll()
{
	static constexpr auto PlayRate{1.3f};

	StartRolling(PlayRate);
}

void AAlsCharacterExample::Input_OnRotationMode()
{
	SetDesiredRotationMode(GetDesiredRotationMode() == AlsRotationModeTags::VelocityDirection
		                       ? AlsRotationModeTags::ViewDirection
		                       : AlsRotationModeTags::VelocityDirection);
}

void AAlsCharacterExample::Input_OnViewMode()
{
	SetViewMode(GetViewMode() == AlsViewModeTags::ThirdPerson ? AlsViewModeTags::FirstPerson : AlsViewModeTags::ThirdPerson);
}

// ReSharper disable once CppMemberFunctionMayBeConst
void AAlsCharacterExample::Input_OnSwitchShoulder()
{
	Camera->SetRightShoulder(!Camera->IsRightShoulder());
}

void AAlsCharacterExample::DisplayDebug(UCanvas* Canvas, const FDebugDisplayInfo& DisplayInfo, float& Unused, float& VerticalLocation)
{
	if (Camera->IsActive())
	{
		Camera->DisplayDebug(Canvas, DisplayInfo, VerticalLocation);
	}

	Super::DisplayDebug(Canvas, DisplayInfo, Unused, VerticalLocation);
}
