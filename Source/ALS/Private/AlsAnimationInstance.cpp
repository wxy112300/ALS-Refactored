#include "AlsAnimationInstance.h"

#include "AlsAnimationInstanceProxy.h"
#include "AlsCharacter.h"
#include "DrawDebugHelpers.h"
#include "Components/CapsuleComponent.h"
#include "Curves/CurveFloat.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Settings/AlsAnimationInstanceSettings.h"
#include "Utility/AlsConstants.h"
#include "Utility/AlsMacros.h"
#include "Utility/AlsUtility.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AlsAnimationInstance)

void UAlsAnimationInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	Character = Cast<AAlsCharacter>(GetOwningActor());

#if WITH_EDITOR
	if (!GetWorld()->IsGameWorld() && !IsValid(Character))
	{
		// Use default objects for editor preview.
		Character = GetMutableDefault<AAlsCharacter>();
	}
#endif
}

void UAlsAnimationInstance::NativeBeginPlay()
{
	Super::NativeBeginPlay();

	ALS_ENSURE(IsValid(Settings));
	ALS_ENSURE(IsValid(Character));
}

void UAlsAnimationInstance::NativeUpdateAnimation(const float DeltaTime)
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::NativeUpdateAnimation()"),
	                            STAT_UAlsAnimationInstance_NativeUpdateAnimation, STATGROUP_Als)

	Super::NativeUpdateAnimation(DeltaTime);

	if (!IsValid(Settings) || !IsValid(Character))
	{
		return;
	}

	auto* Mesh{GetSkelMeshComponent()};

	if (Mesh->IsUsingAbsoluteRotation() && IsValid(Mesh->GetAttachParent()))
	{
		const auto& ParentTransform{Mesh->GetAttachParent()->GetComponentTransform()};

		// Manually synchronize mesh rotation with character rotation.

		Mesh->MoveComponent(FVector::ZeroVector, ParentTransform.GetRotation() * Character->GetBaseRotationOffset(), false);

		// Re-cache proxy transforms to match the modified mesh transform.

		const auto& Proxy{GetProxyOnGameThread<FAnimInstanceProxy>()};
		const_cast<FTransform&>(Proxy.GetComponentTransform()) = Mesh->GetComponentTransform();
		const_cast<FTransform&>(Proxy.GetComponentRelativeTransform()) = Mesh->GetRelativeTransform();
		const_cast<FTransform&>(Proxy.GetActorTransform()) = Character->GetActorTransform();
	}

#if WITH_EDITORONLY_DATA && ENABLE_DRAW_DEBUG
	bDisplayDebugTraces = UAlsUtility::ShouldDisplayDebugForActor(Character, UAlsConstants::TracesDebugDisplayName());
#endif

	ViewMode = Character->GetViewMode();
	LocomotionMode = Character->GetLocomotionMode();
	RotationMode = Character->GetRotationMode();
	Stance = Character->GetStance();
	Gait = Character->GetGait();
	OverlayMode = Character->GetOverlayMode();

	if (LocomotionAction != Character->GetLocomotionAction())
	{
		LocomotionAction = Character->GetLocomotionAction();
		ResetGroundedEntryMode();
	}

	RefreshMovementBaseOnGameThread();
	RefreshViewOnGameThread();
	RefreshLocomotionOnGameThread();
	RefreshInAirOnGameThread();
	RefreshFeetOnGameThread();
	RefreshRagdollingOnGameThread();
}

void UAlsAnimationInstance::NativeThreadSafeUpdateAnimation(const float DeltaTime)
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::NativeThreadSafeUpdateAnimation()"),
	                            STAT_UAlsAnimationInstance_NativeThreadSafeUpdateAnimation, STATGROUP_Als)

	Super::NativeThreadSafeUpdateAnimation(DeltaTime);

	if (!IsValid(Settings) || !IsValid(Character))
	{
		return;
	}

	DynamicTransitionsState.bUpdatedThisFrame = false;
	RotateInPlaceState.bUpdatedThisFrame = false;
	TurnInPlaceState.bUpdatedThisFrame = false;

	RefreshLayering();
	RefreshPose();
	RefreshView(DeltaTime);
	RefreshFeet(DeltaTime);
	RefreshTransitions();
}

void UAlsAnimationInstance::NativePostUpdateAnimation()
{
	// Can't use UAnimationInstance::NativePostEvaluateAnimation() instead this function, as it will not be called if
	// USkinnedMeshComponent::VisibilityBasedAnimTickOption is set to EVisibilityBasedAnimTickOption::AlwaysTickPose.

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::NativePostUpdateAnimation()"),
	                            STAT_UAlsAnimationInstance_NativePostUpdateAnimation, STATGROUP_Als)

	if (!IsValid(Settings) || !IsValid(Character))
	{
		return;
	}

	PlayQueuedTransitionAnimation();
	PlayQueuedTurnInPlaceAnimation();
	StopQueuedTransitionAndTurnInPlaceAnimations();

#if WITH_EDITORONLY_DATA && ENABLE_DRAW_DEBUG
	if (!bPendingUpdate)
	{
		for (const auto& DisplayDebugTraceFunction : DisplayDebugTracesQueue)
		{
			DisplayDebugTraceFunction();
		}
	}

	DisplayDebugTracesQueue.Reset();
#endif

	bPendingUpdate = false;
}

FAnimInstanceProxy* UAlsAnimationInstance::CreateAnimInstanceProxy()
{
	return new FAlsAnimationInstanceProxy{this};
}

FAlsControlRigInput UAlsAnimationInstance::GetControlRigInput() const
{
	return {
		!IsValid(Settings) || Settings->General.bUseHandIkBones,
		!IsValid(Settings) || Settings->General.bUseFootIkBones,
		GroundedState.VelocityBlend.ForwardAmount,
		GroundedState.VelocityBlend.BackwardAmount,
		SpineState.YawAngle,
		FeetState.Left.IkRotation,
		FeetState.Left.IkLocation,
		FeetState.Left.IkAmount,
		FeetState.Right.IkRotation,
		FeetState.Right.IkLocation,
		FeetState.Right.IkAmount,
		FeetState.MinMaxPelvisOffsetZ,
	};
}

void UAlsAnimationInstance::RefreshMovementBaseOnGameThread()
{
	const auto& BasedMovement{Character->GetBasedMovement()};

	if (BasedMovement.MovementBase != MovementBase.Primitive || BasedMovement.BoneName != MovementBase.BoneName)
	{
		MovementBase.Primitive = BasedMovement.MovementBase;
		MovementBase.BoneName = BasedMovement.BoneName;
		MovementBase.bBaseChanged = true;
	}
	else
	{
		MovementBase.bBaseChanged = false;
	}

	MovementBase.bHasRelativeLocation = BasedMovement.HasRelativeLocation();
	MovementBase.bHasRelativeRotation = MovementBase.bHasRelativeLocation && BasedMovement.bRelativeRotation;

	const auto PreviousRotation{MovementBase.Rotation};

	MovementBaseUtility::GetMovementBaseTransform(BasedMovement.MovementBase, BasedMovement.BoneName,
	                                              MovementBase.Location, MovementBase.Rotation);

	MovementBase.DeltaRotation = MovementBase.bHasRelativeLocation && !MovementBase.bBaseChanged
		                             ? (MovementBase.Rotation * PreviousRotation.Inverse()).Rotator()
		                             : FRotator::ZeroRotator;
}

void UAlsAnimationInstance::RefreshLayering()
{
	const auto& Curves{GetProxyOnAnyThread<FAlsAnimationInstanceProxy>().GetAnimationCurves(EAnimCurveType::AttributeCurve)};

	static const auto GetCurveValue{
		[](const TMap<FName, float>& Curves, const FName& CurveName) -> float
		{
			const auto* Value{Curves.Find(CurveName)};

			return Value != nullptr ? *Value : 0.0f;
		}
	};

	LayeringState.HeadBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerHeadCurveName());
	LayeringState.HeadAdditiveBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerHeadAdditiveCurveName());
	LayeringState.HeadSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerHeadSlotCurveName());

	// The mesh space blend will always be 1 unless the local space blend is 1.

	LayeringState.ArmLeftBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmLeftCurveName());
	LayeringState.ArmLeftAdditiveBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmLeftAdditiveCurveName());
	LayeringState.ArmLeftSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmLeftSlotCurveName());
	LayeringState.ArmLeftLocalSpaceBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmLeftLocalSpaceCurveName());
	LayeringState.ArmLeftMeshSpaceBlendAmount = !FAnimWeight::IsFullWeight(LayeringState.ArmLeftLocalSpaceBlendAmount);

	// The mesh space blend will always be 1 unless the local space blend is 1.

	LayeringState.ArmRightBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmRightCurveName());
	LayeringState.ArmRightAdditiveBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmRightAdditiveCurveName());
	LayeringState.ArmRightSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmRightSlotCurveName());
	LayeringState.ArmRightLocalSpaceBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmRightLocalSpaceCurveName());
	LayeringState.ArmRightMeshSpaceBlendAmount = !FAnimWeight::IsFullWeight(LayeringState.ArmRightLocalSpaceBlendAmount);

	LayeringState.HandLeftBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerHandLeftCurveName());
	LayeringState.HandRightBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerHandRightCurveName());

	LayeringState.SpineBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerSpineCurveName());
	LayeringState.SpineAdditiveBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerSpineAdditiveCurveName());
	LayeringState.SpineSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerSpineSlotCurveName());

	LayeringState.PelvisBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerPelvisCurveName());
	LayeringState.PelvisSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerPelvisSlotCurveName());

	LayeringState.LegsBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerLegsCurveName());
	LayeringState.LegsSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerLegsSlotCurveName());
}

void UAlsAnimationInstance::RefreshPose()
{
	const auto& Curves{GetProxyOnAnyThread<FAlsAnimationInstanceProxy>().GetAnimationCurves(EAnimCurveType::AttributeCurve)};

	static const auto GetCurveValue{
		[](const TMap<FName, float>& Curves, const FName& CurveName) -> float
		{
			const auto* Value{Curves.Find(CurveName)};

			return Value != nullptr ? *Value : 0.0f;
		}
	};

	PoseState.GroundedAmount = GetCurveValue(Curves, UAlsConstants::PoseGroundedCurveName());
	PoseState.InAirAmount = GetCurveValue(Curves, UAlsConstants::PoseInAirCurveName());

	PoseState.StandingAmount = GetCurveValue(Curves, UAlsConstants::PoseStandingCurveName());
	PoseState.CrouchingAmount = GetCurveValue(Curves, UAlsConstants::PoseCrouchingCurveName());

	PoseState.MovingAmount = GetCurveValue(Curves, UAlsConstants::PoseMovingCurveName());

	PoseState.GaitAmount = FMath::Clamp(GetCurveValue(Curves, UAlsConstants::PoseGaitCurveName()), 0.0f, 3.0f);
	PoseState.GaitWalkingAmount = UAlsMath::Clamp01(PoseState.GaitAmount);
	PoseState.GaitRunningAmount = UAlsMath::Clamp01(PoseState.GaitAmount - 1.0f);
	PoseState.GaitSprintingAmount = UAlsMath::Clamp01(PoseState.GaitAmount - 2.0f);

	// Use the grounded pose curve value to "unweight" the gait pose curve. This is used to
	// instantly get the full gait value from the very beginning of transitions to grounded states.

	PoseState.UnweightedGaitAmount = PoseState.GroundedAmount > UE_SMALL_NUMBER
		                                 ? PoseState.GaitAmount / PoseState.GroundedAmount
		                                 : PoseState.GaitAmount;

	PoseState.UnweightedGaitWalkingAmount = UAlsMath::Clamp01(PoseState.UnweightedGaitAmount);
	PoseState.UnweightedGaitRunningAmount = UAlsMath::Clamp01(PoseState.UnweightedGaitAmount - 1.0f);
	PoseState.UnweightedGaitSprintingAmount = UAlsMath::Clamp01(PoseState.UnweightedGaitAmount - 2.0f);
}

void UAlsAnimationInstance::RefreshViewOnGameThread()
{
	check(IsInGameThread())

	const auto& View{Character->GetViewState()};

	ViewState.Rotation = View.Rotation;
	ViewState.YawSpeed = View.YawSpeed;
}

void UAlsAnimationInstance::RefreshView(const float DeltaTime)
{
	if (!LocomotionAction.IsValid())
	{
		ViewState.YawAngle = FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(ViewState.Rotation.Yaw - LocomotionState.Rotation.Yaw));
		ViewState.PitchAngle = FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(ViewState.Rotation.Pitch - LocomotionState.Rotation.Pitch));

		ViewState.PitchAmount = 0.5f - ViewState.PitchAngle / 180.0f;
	}

	const auto ViewAmount{1.0f - GetCurveValueClamped01(UAlsConstants::ViewBlockCurveName())};
	const auto AimingAmount{GetCurveValueClamped01(UAlsConstants::AllowAimingCurveName())};

	ViewState.LookAmount = ViewAmount * (1.0f - AimingAmount);

	RefreshSpine(ViewAmount * AimingAmount, DeltaTime);
}

bool UAlsAnimationInstance::IsSpineRotationAllowed()
{
	return RotationMode == AlsRotationModeTags::Aiming;
}

void UAlsAnimationInstance::RefreshSpine(const float SpineBlendAmount, const float DeltaTime)
{
	if (SpineState.bSpineRotationAllowed != IsSpineRotationAllowed())
	{
		SpineState.bSpineRotationAllowed = !SpineState.bSpineRotationAllowed;

		if (SpineState.bSpineRotationAllowed)
		{
			// Remap SpineAmount from the [SpineAmount, 1] range to [0, 1] so that lerp between new LastYawAngle
			// and ViewState.YawAngle with an alpha equal to SpineAmount still results in CurrentYawAngle.

			if (FAnimWeight::IsFullWeight(SpineState.SpineAmount))
			{
				SpineState.SpineAmountScale = 1.0f;
				SpineState.SpineAmountBias = 0.0f;
			}
			else
			{
				SpineState.SpineAmountScale = 1.0f / (1.0f - SpineState.SpineAmount);
				SpineState.SpineAmountBias = -SpineState.SpineAmount * SpineState.SpineAmountScale;
			}
		}
		else
		{
			// Remap SpineAmount from the [0, SpineAmount] range to [0, 1] so that lerp between 0
			// and LastYawAngle with an alpha equal to SpineAmount still results in CurrentYawAngle.

			SpineState.SpineAmountScale = !FAnimWeight::IsRelevant(SpineState.SpineAmount)
				                              ? 1.0f
				                              : 1.0f / SpineState.SpineAmount;

			SpineState.SpineAmountBias = 0.0f;
		}

		SpineState.LastYawAngle = SpineState.CurrentYawAngle;
		SpineState.LastActorYawAngle = UE_REAL_TO_FLOAT(LocomotionState.Rotation.Yaw);
	}

	if (SpineState.bSpineRotationAllowed)
	{
		if (bPendingUpdate || FAnimWeight::IsFullWeight(SpineState.SpineAmount))
		{
			SpineState.SpineAmount = 1.0f;
			SpineState.CurrentYawAngle = ViewState.YawAngle;
		}
		else
		{
			static constexpr auto InterpolationSpeed{20.0f};

			SpineState.SpineAmount = UAlsMath::ExponentialDecay(SpineState.SpineAmount, 1.0f, DeltaTime, InterpolationSpeed);

			SpineState.CurrentYawAngle = UAlsMath::LerpAngle(SpineState.LastYawAngle, ViewState.YawAngle,
			                                                 SpineState.SpineAmount * SpineState.SpineAmountScale +
			                                                 SpineState.SpineAmountBias);
		}
	}
	else
	{
		if (bPendingUpdate || !FAnimWeight::IsRelevant(SpineState.SpineAmount))
		{
			SpineState.SpineAmount = 0.0f;
			SpineState.CurrentYawAngle = 0.0f;
		}
		else
		{
			static constexpr auto InterpolationSpeed{1.0f};
			static constexpr auto ReferenceViewYawSpeed{40.0f};

			// Increase the interpolation speed when the camera rotates quickly,
			// otherwise the spine rotation may lag too much behind the actor rotation.

			const auto InterpolationSpeedMultiplier{FMath::Max(1.0f, FMath::Abs(ViewState.YawSpeed) / ReferenceViewYawSpeed)};

			SpineState.SpineAmount = UAlsMath::ExponentialDecay(SpineState.SpineAmount, 0.0f, DeltaTime,
			                                                    InterpolationSpeed * InterpolationSpeedMultiplier);

			if (MovementBase.bHasRelativeRotation)
			{
				// Offset the angle to keep it relative to the movement base.
				SpineState.LastActorYawAngle = FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(
					SpineState.LastActorYawAngle + MovementBase.DeltaRotation.Yaw));
			}

			// Offset the spine rotation to keep it unchanged in world space to achieve a smoother spine rotation when aiming stops.

			auto YawAngleOffset{FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(SpineState.LastActorYawAngle - LocomotionState.Rotation.Yaw))};

			// Keep the offset within 30 degrees, otherwise the spine rotation may lag too much behind the actor rotation.

			static constexpr auto MaxYawAngleOffset{30.0f};
			YawAngleOffset = FMath::Clamp(YawAngleOffset, -MaxYawAngleOffset, MaxYawAngleOffset);

			SpineState.LastActorYawAngle = FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(YawAngleOffset + LocomotionState.Rotation.Yaw));

			SpineState.CurrentYawAngle = UAlsMath::LerpAngle(0.0f, SpineState.LastYawAngle + YawAngleOffset,
			                                                 SpineState.SpineAmount * SpineState.SpineAmountScale +
			                                                 SpineState.SpineAmountBias);
		}
	}

	SpineState.YawAngle = UAlsMath::LerpAngle(0.0f, SpineState.CurrentYawAngle, SpineBlendAmount);
}

void UAlsAnimationInstance::InitializeLook()
{
	LookState.bInitializationRequired = true;
}

void UAlsAnimationInstance::RefreshLook()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshLook()"), STAT_UAlsAnimationInstance_RefreshLook, STATGROUP_Als)

	if (!IsValid(Settings))
	{
		return;
	}

	const auto ActorYawAngle{UE_REAL_TO_FLOAT(LocomotionState.Rotation.Yaw)};

	if (MovementBase.bHasRelativeRotation)
	{
		// Offset the angle to keep it relative to the movement base.
		LookState.WorldYawAngle = FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(LookState.WorldYawAngle + MovementBase.DeltaRotation.Yaw));
	}

	float TargetYawAngle;
	float TargetPitchAngle;
	float InterpolationSpeed;

	if (RotationMode == AlsRotationModeTags::VelocityDirection)
	{
		// Look towards input direction.

		TargetYawAngle = FRotator3f::NormalizeAxis(
			(LocomotionState.bHasInput ? LocomotionState.InputYawAngle : LocomotionState.TargetYawAngle) - ActorYawAngle);

		TargetPitchAngle = 0.0f;
		InterpolationSpeed = Settings->View.LookTowardsInputYawAngleInterpolationSpeed;
	}
	else
	{
		// Look towards view direction.

		TargetYawAngle = ViewState.YawAngle;
		TargetPitchAngle = ViewState.PitchAngle;
		InterpolationSpeed = Settings->View.LookTowardsCameraRotationInterpolationSpeed;
	}

	if (LookState.bInitializationRequired || InterpolationSpeed <= 0.0f)
	{
		LookState.YawAngle = TargetYawAngle;
		LookState.PitchAngle = TargetPitchAngle;

		LookState.bInitializationRequired = false;
	}
	else
	{
		const auto YawAngle{FRotator3f::NormalizeAxis(LookState.WorldYawAngle - ActorYawAngle)};
		auto DeltaYawAngle{FRotator3f::NormalizeAxis(TargetYawAngle - YawAngle)};

		if (DeltaYawAngle > 180.0f - UAlsMath::CounterClockwiseRotationAngleThreshold)
		{
			DeltaYawAngle -= 360.0f;
		}
		else if (FMath::Abs(LocomotionState.YawSpeed) > UE_SMALL_NUMBER && FMath::Abs(TargetYawAngle) > 90.0f)
		{
			// When interpolating yaw angle, favor the character rotation direction, over the shortest rotation
			// direction, so that the rotation of the head remains synchronized with the rotation of the body.

			DeltaYawAngle = LocomotionState.YawSpeed > 0.0f ? FMath::Abs(DeltaYawAngle) : -FMath::Abs(DeltaYawAngle);
		}

		const auto InterpolationAmount{UAlsMath::ExponentialDecay(GetDeltaSeconds(), InterpolationSpeed)};

		LookState.YawAngle = FRotator3f::NormalizeAxis(YawAngle + DeltaYawAngle * InterpolationAmount);
		LookState.PitchAngle = UAlsMath::LerpAngle(LookState.PitchAngle, TargetPitchAngle, InterpolationAmount);
	}

	LookState.WorldYawAngle = FRotator3f::NormalizeAxis(ActorYawAngle + LookState.YawAngle);

	// Separate the yaw angle into 3 separate values. These 3 values are used to improve the
	// blending of the view when rotating completely around the character. This allows to
	// keep the view responsive but still smoothly blend from left to right or right to left.

	LookState.YawForwardAmount = LookState.YawAngle / 360.0f + 0.5f;
	LookState.YawLeftAmount = 0.5f - FMath::Abs(LookState.YawForwardAmount - 0.5f);
	LookState.YawRightAmount = 0.5f + FMath::Abs(LookState.YawForwardAmount - 0.5f);
}

void UAlsAnimationInstance::RefreshLocomotionOnGameThread()
{
	check(IsInGameThread())

	const auto* World{GetWorld()};
	const auto ActorDeltaTime{IsValid(World) ? World->DeltaTimeSeconds * Character->CustomTimeDilation : 0.0f};

	const auto& Locomotion{Character->GetLocomotionState()};

	LocomotionState.bHasInput = Locomotion.bHasInput;
	LocomotionState.InputYawAngle = Locomotion.InputYawAngle;

	LocomotionState.Speed = Locomotion.Speed;
	LocomotionState.Velocity = Locomotion.Velocity;
	LocomotionState.VelocityYawAngle = Locomotion.VelocityYawAngle;

	LocomotionState.Acceleration = ActorDeltaTime > UE_SMALL_NUMBER
		                               ? (Locomotion.Velocity - Locomotion.PreviousVelocity) / ActorDeltaTime
		                               : FVector::ZeroVector;

	const auto* Movement{Character->GetCharacterMovement()};

	LocomotionState.MaxAcceleration = Movement->GetMaxAcceleration();
	LocomotionState.MaxBrakingDeceleration = Movement->GetMaxBrakingDeceleration();
	LocomotionState.WalkableFloorZ = Movement->GetWalkableFloorZ();

	LocomotionState.bMoving = Locomotion.bMoving;

	LocomotionState.bMovingSmooth = (Locomotion.bHasInput && Locomotion.bHasSpeed) ||
	                                Locomotion.Speed > Settings->General.MovingSmoothSpeedThreshold;

	LocomotionState.TargetYawAngle = Locomotion.TargetYawAngle;
	LocomotionState.Location = Locomotion.Location;
	LocomotionState.Rotation = Locomotion.Rotation;
	LocomotionState.RotationQuaternion = Locomotion.Rotation.Quaternion();

	LocomotionState.YawSpeed = ActorDeltaTime > UE_SMALL_NUMBER
		                           ? FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(
			                             Locomotion.Rotation.Yaw - Locomotion.PreviousYawAngle)) / ActorDeltaTime
		                           : 0.0f;

	LocomotionState.Scale = UE_REAL_TO_FLOAT(GetSkelMeshComponent()->GetComponentScale().Z);

	const auto* Capsule{Character->GetCapsuleComponent()};

	LocomotionState.CapsuleRadius = Capsule->GetScaledCapsuleRadius();
	LocomotionState.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
}

void UAlsAnimationInstance::InitializeLean()
{
	LeanState.RightAmount = 0.0f;
	LeanState.ForwardAmount = 0.0f;
}

void UAlsAnimationInstance::InitializeGrounded()
{
	GroundedState.VelocityBlend.bInitializationRequired = true;
}

void UAlsAnimationInstance::RefreshGrounded()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshGrounded()"),
	                            STAT_UAlsAnimationInstance_RefreshGrounded, STATGROUP_Als)

	if (!IsValid(Settings))
	{
		return;
	}

	RefreshVelocityBlend();
	RefreshGroundedLean();
}

FVector3f UAlsAnimationInstance::GetRelativeVelocity() const
{
	return FVector3f{LocomotionState.RotationQuaternion.UnrotateVector(LocomotionState.Velocity)};
}

FVector2f UAlsAnimationInstance::GetRelativeAccelerationAmount() const
{
	// This value represents the current amount of acceleration / deceleration relative to the
	// character rotation. It is normalized to a range of -1 to 1 so that -1 equals the max
	// braking deceleration and 1 equals the max acceleration of the character movement component.

	const FVector3f RelativeAcceleration{LocomotionState.RotationQuaternion.UnrotateVector(LocomotionState.Acceleration)};

	const auto MaxAcceleration{
		(LocomotionState.Acceleration | LocomotionState.Velocity) >= 0.0f
			? LocomotionState.MaxAcceleration
			: LocomotionState.MaxBrakingDeceleration
	};

	return FVector2f{UAlsMath::ClampMagnitude01(RelativeAcceleration / MaxAcceleration)};
}

void UAlsAnimationInstance::RefreshVelocityBlend()
{
	// Calculate and interpolate the velocity blend amounts. This value represents the velocity amount of
	// the character in each direction (normalized so that diagonals equal 0.5 for each direction) and is
	// used in a blend multi node to produce better directional blending than a standard blend space.

	auto& VelocityBlend{GroundedState.VelocityBlend};

	const auto RelativeVelocityDirection{GetRelativeVelocity().GetSafeNormal()};

	const auto TargetVelocityBlend{
		RelativeVelocityDirection /
		(FMath::Abs(RelativeVelocityDirection.X) + FMath::Abs(RelativeVelocityDirection.Y) + FMath::Abs(RelativeVelocityDirection.Z))
	};

	if (VelocityBlend.bInitializationRequired)
	{
		VelocityBlend.bInitializationRequired = false;

		VelocityBlend.ForwardAmount = UAlsMath::Clamp01(TargetVelocityBlend.X);
		VelocityBlend.BackwardAmount = FMath::Abs(FMath::Clamp(TargetVelocityBlend.X, -1.0f, 0.0f));
		VelocityBlend.LeftAmount = FMath::Abs(FMath::Clamp(TargetVelocityBlend.Y, -1.0f, 0.0f));
		VelocityBlend.RightAmount = UAlsMath::Clamp01(TargetVelocityBlend.Y);
	}
	else
	{
		const auto DeltaTime{GetDeltaSeconds()};

		VelocityBlend.ForwardAmount = FMath::FInterpTo(VelocityBlend.ForwardAmount,
		                                               UAlsMath::Clamp01(TargetVelocityBlend.X),
		                                               DeltaTime, Settings->Grounded.VelocityBlendInterpolationSpeed);

		VelocityBlend.BackwardAmount = FMath::FInterpTo(VelocityBlend.BackwardAmount,
		                                                FMath::Abs(FMath::Clamp(TargetVelocityBlend.X, -1.0f, 0.0f)),
		                                                DeltaTime, Settings->Grounded.VelocityBlendInterpolationSpeed);

		VelocityBlend.LeftAmount = FMath::FInterpTo(VelocityBlend.LeftAmount,
		                                            FMath::Abs(FMath::Clamp(TargetVelocityBlend.Y, -1.0f, 0.0f)),
		                                            DeltaTime, Settings->Grounded.VelocityBlendInterpolationSpeed);

		VelocityBlend.RightAmount = FMath::FInterpTo(VelocityBlend.RightAmount,
		                                             UAlsMath::Clamp01(TargetVelocityBlend.Y),
		                                             DeltaTime, Settings->Grounded.VelocityBlendInterpolationSpeed);
	}
}

void UAlsAnimationInstance::RefreshGroundedLean()
{
	const auto TargetLeanAmount{GetRelativeAccelerationAmount()};

	if (bPendingUpdate)
	{
		LeanState.RightAmount = TargetLeanAmount.Y;
		LeanState.ForwardAmount = TargetLeanAmount.X;
	}
	else
	{
		const auto DeltaTime{GetDeltaSeconds()};

		LeanState.RightAmount = FMath::FInterpTo(LeanState.RightAmount, TargetLeanAmount.Y,
		                                         DeltaTime, Settings->General.LeanInterpolationSpeed);

		LeanState.ForwardAmount = FMath::FInterpTo(LeanState.ForwardAmount, TargetLeanAmount.X,
		                                           DeltaTime, Settings->General.LeanInterpolationSpeed);
	}
}

void UAlsAnimationInstance::RefreshGroundedMovement()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshGroundedMovement()"),
	                            STAT_UAlsAnimationInstance_RefreshGroundedMovement, STATGROUP_Als)

	if (!IsValid(Settings))
	{
		return;
	}

	GroundedState.HipsDirectionLockAmount = FMath::Clamp(GetCurveValue(UAlsConstants::HipsDirectionLockCurveName()), -1.0f, 1.0f);

	const auto ViewRelativeVelocityYawAngle{
		FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(LocomotionState.VelocityYawAngle - ViewState.Rotation.Yaw))
	};

	RefreshMovementDirection(ViewRelativeVelocityYawAngle);
	RefreshRotationYawOffsets(ViewRelativeVelocityYawAngle);
}

void UAlsAnimationInstance::RefreshMovementDirection(const float ViewRelativeVelocityYawAngle)
{
	// Calculate the movement direction. This value represents the direction the character is moving relative to the camera during
	// the view direction and aiming rotation modes and is used in the cycle blending to blend to the appropriate directional states.

	if (RotationMode == AlsRotationModeTags::VelocityDirection || Gait == AlsGaitTags::Sprinting)
	{
		GroundedState.MovementDirection = EAlsMovementDirection::Forward;
		return;
	}

	static constexpr auto ForwardHalfAngle{70.0f};
	static constexpr auto AngleThreshold{5.0f};

	GroundedState.MovementDirection = UAlsMath::CalculateMovementDirection(
		ViewRelativeVelocityYawAngle, ForwardHalfAngle, AngleThreshold);
}

void UAlsAnimationInstance::RefreshRotationYawOffsets(const float ViewRelativeVelocityYawAngle)
{
	// Rotation yaw offsets influence the rotation yaw offset curve in the animation
	// graph and is used to offset the character's rotation for more natural movement.
	// The curves allow us to precisely control the offset for each movement direction.

	auto& RotationYawOffsets{GroundedState.RotationYawOffsets};

	RotationYawOffsets.ForwardAngle = Settings->Grounded.RotationYawOffsetForwardCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
	RotationYawOffsets.BackwardAngle = Settings->Grounded.RotationYawOffsetBackwardCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
	RotationYawOffsets.LeftAngle = Settings->Grounded.RotationYawOffsetLeftCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
	RotationYawOffsets.RightAngle = Settings->Grounded.RotationYawOffsetRightCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
}

void UAlsAnimationInstance::InitializeStandingMovement()
{
	StandingState.SprintTime = 0.0f;
	StandingState.bPivotActive = false;
}

void UAlsAnimationInstance::RefreshStandingMovement()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshStandingMovement()"),
	                            STAT_UAlsAnimationInstance_RefreshStandingMovement, STATGROUP_Als)

	if (!IsValid(Settings))
	{
		return;
	}

	const auto Speed{LocomotionState.Speed / LocomotionState.Scale};

	// Calculate the stride blend amount. This value is used within the blend spaces to scale the stride (distance feet travel)
	// so that the character can walk or run at different movement speeds. It also allows the walk or run gait animations to
	// blend independently while still matching the animation speed to the movement speed, preventing the character from needing
	// to play a half walk + half run blend. The curves are used to map the stride amount to the speed for maximum control.

	StandingState.StrideBlendAmount = FMath::Lerp(Settings->Standing.StrideBlendAmountWalkCurve->GetFloatValue(Speed),
	                                              Settings->Standing.StrideBlendAmountRunCurve->GetFloatValue(Speed),
	                                              PoseState.UnweightedGaitRunningAmount);

	// Calculate the walk run blend amount. This value is used within the blend spaces to blend between walking and running.

	StandingState.WalkRunBlendAmount = Gait == AlsGaitTags::Walking ? 0.0f : 1.0f;

	// Calculate the standing play rate by dividing the character's speed by the animated speed for each gait.
	// The interpolation is determined by the gait amount curve that exists on every locomotion cycle so that
	// the play rate is always in sync with the currently blended animation. The value is also divided by the
	// stride blend and the capsule scale so that the play rate increases as the stride or scale gets smaller.

	const auto WalkRunSpeedAmount{
		FMath::Lerp(Speed / Settings->Standing.AnimatedWalkSpeed,
		            Speed / Settings->Standing.AnimatedRunSpeed,
		            PoseState.UnweightedGaitRunningAmount)
	};

	const auto WalkRunSprintSpeedAmount{
		FMath::Lerp(WalkRunSpeedAmount,
		            Speed / Settings->Standing.AnimatedSprintSpeed,
		            PoseState.UnweightedGaitSprintingAmount)
	};

	// Do not let the play rate be exactly zero, otherwise animation notifies
	// may start to be triggered every frame until the play rate is changed.

	// TODO Check the need for this hack in future engine versions.

	StandingState.PlayRate = FMath::Clamp(WalkRunSprintSpeedAmount / StandingState.StrideBlendAmount, UE_KINDA_SMALL_NUMBER, 3.0f);

	StandingState.SprintBlockAmount = GetCurveValueClamped01(UAlsConstants::SprintBlockCurveName());

	if (Gait != AlsGaitTags::Sprinting)
	{
		StandingState.SprintTime = 0.0f;
		StandingState.SprintAccelerationAmount = 0.0f;
		return;
	}

	// Use the relative acceleration as the sprint relative acceleration if less than 0.5 seconds has
	// elapsed since the start of the sprint, otherwise set the sprint relative acceleration to zero.
	// This is necessary to apply the acceleration animation only at the beginning of the sprint.

	static constexpr auto SprintTimeThreshold{0.5f};

	StandingState.SprintTime = bPendingUpdate
		                           ? SprintTimeThreshold
		                           : StandingState.SprintTime + GetDeltaSeconds();

	StandingState.SprintAccelerationAmount = StandingState.SprintTime >= SprintTimeThreshold
		                                         ? 0.0f
		                                         : GetRelativeAccelerationAmount().X;
}

void UAlsAnimationInstance::ActivatePivot()
{
	StandingState.bPivotActive = LocomotionState.Speed < Settings->Standing.PivotActivationSpeedThreshold;
}

void UAlsAnimationInstance::RefreshCrouchingMovement()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshCrouchingMovement()"),
	                            STAT_UAlsAnimationInstance_RefreshCrouchingMovement, STATGROUP_Als)

	if (!IsValid(Settings))
	{
		return;
	}

	const auto Speed{LocomotionState.Speed / LocomotionState.Scale};

	CrouchingState.StrideBlendAmount = Settings->Crouching.StrideBlendAmountCurve->GetFloatValue(Speed);

	CrouchingState.PlayRate = FMath::Clamp(
		Speed / (Settings->Crouching.AnimatedCrouchSpeed * CrouchingState.StrideBlendAmount),
		UE_KINDA_SMALL_NUMBER, 2.0f);
}

void UAlsAnimationInstance::RefreshInAirOnGameThread()
{
	check(IsInGameThread())

	InAirState.bJumped = !bPendingUpdate && (InAirState.bJumped || InAirState.bJumpRequested);
	InAirState.bJumpRequested = false;
}

void UAlsAnimationInstance::RefreshInAir()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshInAir()"), STAT_UAlsAnimationInstance_RefreshInAir, STATGROUP_Als)

	if (!IsValid(Settings))
	{
		return;
	}

	if (InAirState.bJumped)
	{
		static constexpr auto ReferenceSpeed{600.0f};
		static constexpr auto MinPlayRate{1.2f};
		static constexpr auto MaxPlayRate{1.5f};

		InAirState.bJumped = false;
		InAirState.JumpPlayRate = UAlsMath::LerpClamped(MinPlayRate, MaxPlayRate, LocomotionState.Speed / ReferenceSpeed);
	}

	// A separate variable for vertical speed is used to determine at what speed the character landed on the ground.

	InAirState.VerticalVelocity = UE_REAL_TO_FLOAT(LocomotionState.Velocity.Z);

	RefreshGroundPrediction();
	RefreshInAirLean();
}

void UAlsAnimationInstance::RefreshGroundPrediction()
{
	// Calculate the ground prediction weight by tracing in the velocity direction to find a walkable surface the character
	// is falling toward and getting the "time" (range from 0 to 1, 1 being maximum, 0 being about to ground) till impact.
	// The ground prediction amount curve is used to control how the time affects the final amount for a smooth blend.

	static constexpr auto VerticalVelocityThreshold{-200.0f};

	if (InAirState.VerticalVelocity > VerticalVelocityThreshold)
	{
		InAirState.GroundPredictionAmount = 0.0f;
		return;
	}

	const auto AllowanceAmount{1.0f - GetCurveValueClamped01(UAlsConstants::GroundPredictionBlockCurveName())};
	if (AllowanceAmount <= UE_KINDA_SMALL_NUMBER)
	{
		InAirState.GroundPredictionAmount = 0.0f;
		return;
	}

	const auto SweepStartLocation{LocomotionState.Location};

	static constexpr auto MinVerticalVelocity{-4000.0f};
	static constexpr auto MaxVerticalVelocity{-200.0f};

	auto VelocityDirection{LocomotionState.Velocity};
	VelocityDirection.Z = FMath::Clamp(VelocityDirection.Z, MinVerticalVelocity, MaxVerticalVelocity);
	VelocityDirection.Normalize();

	static constexpr auto MinSweepDistance{150.0f};
	static constexpr auto MaxSweepDistance{2000.0f};

	const auto SweepVector{
		VelocityDirection * FMath::GetMappedRangeValueClamped(FVector2f{MaxVerticalVelocity, MinVerticalVelocity},
		                                                      {MinSweepDistance, MaxSweepDistance},
		                                                      InAirState.VerticalVelocity) * LocomotionState.Scale
	};

	FHitResult Hit;
	GetWorld()->SweepSingleByChannel(Hit, SweepStartLocation, SweepStartLocation + SweepVector,
	                                 FQuat::Identity, Settings->InAir.GroundPredictionSweepChannel,
	                                 FCollisionShape::MakeCapsule(LocomotionState.CapsuleRadius, LocomotionState.CapsuleHalfHeight),
	                                 {__FUNCTION__, false, Character}, Settings->InAir.GroundPredictionSweepResponses);

	const auto bGroundValid{Hit.IsValidBlockingHit() && Hit.ImpactNormal.Z >= LocomotionState.WalkableFloorZ};

#if WITH_EDITORONLY_DATA && ENABLE_DRAW_DEBUG
	if (bDisplayDebugTraces)
	{
		if (IsInGameThread())
		{
			UAlsUtility::DrawDebugSweepSingleCapsule(GetWorld(), Hit.TraceStart, Hit.TraceEnd, FRotator::ZeroRotator,
			                                         LocomotionState.CapsuleRadius, LocomotionState.CapsuleHalfHeight,
			                                         bGroundValid, Hit, {0.25f, 0.0f, 1.0f}, {0.75f, 0.0f, 1.0f});
		}
		else
		{
			DisplayDebugTracesQueue.Emplace([this, Hit, bGroundValid]
				{
					UAlsUtility::DrawDebugSweepSingleCapsule(GetWorld(), Hit.TraceStart, Hit.TraceEnd, FRotator::ZeroRotator,
					                                         LocomotionState.CapsuleRadius, LocomotionState.CapsuleHalfHeight,
					                                         bGroundValid, Hit, {0.25f, 0.0f, 1.0f}, {0.75f, 0.0f, 1.0f});
				}
			);
		}
	}
#endif

	InAirState.GroundPredictionAmount = bGroundValid
		                                    ? Settings->InAir.GroundPredictionAmountCurve->GetFloatValue(Hit.Time) * AllowanceAmount
		                                    : 0.0f;
}

void UAlsAnimationInstance::RefreshInAirLean()
{
	// Use the relative velocity direction and amount to determine how much the character should lean
	// while in air. The lean amount curve gets the vertical velocity and is used as a multiplier to
	// smoothly reverse the leaning direction when transitioning from moving upwards to moving downwards.

	static constexpr auto ReferenceSpeed{350.0f};

	const auto TargetLeanAmount{
		GetRelativeVelocity() / ReferenceSpeed * Settings->InAir.LeanAmountCurve->GetFloatValue(InAirState.VerticalVelocity)
	};

	if (bPendingUpdate)
	{
		LeanState.RightAmount = TargetLeanAmount.Y;
		LeanState.ForwardAmount = TargetLeanAmount.X;
	}
	else
	{
		const auto DeltaTime{GetDeltaSeconds()};

		LeanState.RightAmount = FMath::FInterpTo(LeanState.RightAmount, TargetLeanAmount.Y,
		                                         DeltaTime, Settings->General.LeanInterpolationSpeed);

		LeanState.ForwardAmount = FMath::FInterpTo(LeanState.ForwardAmount, TargetLeanAmount.X,
		                                           DeltaTime, Settings->General.LeanInterpolationSpeed);
	}
}

void UAlsAnimationInstance::InhibitFootLockForOneFrame()
{
	FeetState.bInhibitFootLockForOneFrame = true;
}

void UAlsAnimationInstance::RefreshFeetOnGameThread()
{
	check(IsInGameThread())

	const auto* Mesh{GetSkelMeshComponent()};

	const auto FootLeftTargetTransform{
		Mesh->GetSocketTransform(Settings->General.bUseFootIkBones
			                         ? UAlsConstants::FootLeftIkBoneName()
			                         : UAlsConstants::FootLeftVirtualBoneName())
	};

	FeetState.Left.TargetLocation = FootLeftTargetTransform.GetLocation();
	FeetState.Left.TargetRotation = FootLeftTargetTransform.GetRotation();

	const auto FootRightTargetTransform{
		Mesh->GetSocketTransform(Settings->General.bUseFootIkBones
			                         ? UAlsConstants::FootRightIkBoneName()
			                         : UAlsConstants::FootRightVirtualBoneName())
	};

	FeetState.Right.TargetLocation = FootRightTargetTransform.GetLocation();
	FeetState.Right.TargetRotation = FootRightTargetTransform.GetRotation();
}

void UAlsAnimationInstance::RefreshFeet(const float DeltaTime)
{
	FeetState.FootPlantedAmount = FMath::Clamp(GetCurveValue(UAlsConstants::FootPlantedCurveName()), -1.0f, 1.0f);
	FeetState.FeetCrossingAmount = GetCurveValueClamped01(UAlsConstants::FeetCrossingCurveName());

	FeetState.MinMaxPelvisOffsetZ = FVector2f::ZeroVector;

	const auto ComponentTransformInverse{GetProxyOnAnyThread<FAnimInstanceProxy>().GetComponentTransform().Inverse()};

	RefreshFoot(FeetState.Left, UAlsConstants::FootLeftIkCurveName(), UAlsConstants::FootLeftLockCurveName(),
	            Settings->Feet.LeftFootLimits, ComponentTransformInverse, DeltaTime);

	RefreshFoot(FeetState.Right, UAlsConstants::FootRightIkCurveName(), UAlsConstants::FootRightLockCurveName(),
	            Settings->Feet.RightFootLimits, ComponentTransformInverse, DeltaTime);

	const auto ScaleInverse{1.0f / LocomotionState.Scale};

	FeetState.MinMaxPelvisOffsetZ.X = UE_REAL_TO_FLOAT(
		FMath::Min(FeetState.Left.OffsetTargetLocationZ, FeetState.Right.OffsetTargetLocationZ) * ScaleInverse);

	FeetState.MinMaxPelvisOffsetZ.Y = UE_REAL_TO_FLOAT(
		FMath::Max(FeetState.Left.OffsetTargetLocationZ, FeetState.Right.OffsetTargetLocationZ) * ScaleInverse);

	FeetState.bInhibitFootLockForOneFrame = false;
}

void UAlsAnimationInstance::RefreshFoot(FAlsFootState& FootState, const FName& FootIkCurveName,
                                        const FName& FootLockCurveName, const FAlsFootLimitsSettings& LimitsSettings,
                                        const FTransform& ComponentTransformInverse, const float DeltaTime) const
{
	FootState.IkAmount = GetCurveValueClamped01(FootIkCurveName);

	ProcessFootLockTeleport(FootState);

	ProcessFootLockBaseChange(FootState, ComponentTransformInverse);

	auto FinalLocation{FootState.TargetLocation};
	auto FinalRotation{FootState.TargetRotation};

	RefreshFootLock(FootState, FootLockCurveName, ComponentTransformInverse, DeltaTime, FinalLocation, FinalRotation);

	const auto PreviousFinalRotation{FinalRotation};
	RefreshFootOffset(FootState, DeltaTime, FinalLocation, FinalRotation);

	// Prevent the foot from assuming an unnatural pose when on a highly
	// sloped surface by limiting its rotation after applying a foot offset.

	LimitFootRotation(LimitsSettings, PreviousFinalRotation, FinalRotation);

	FootState.IkLocation = ComponentTransformInverse.TransformPosition(FinalLocation);
	FootState.IkRotation = ComponentTransformInverse.TransformRotation(FinalRotation);
}

void UAlsAnimationInstance::ProcessFootLockTeleport(FAlsFootState& FootState) const
{
	// Due to network smoothing, we assume that teleportation occurs over a short period of time, not
	// in one frame, since after accepting the teleportation event, the character can still be moved for
	// some indefinite time, and this must be taken into account in order to avoid foot locking glitches.

	if (bPendingUpdate || GetWorld()->TimeSince(TeleportedTime) > 0.2f ||
	    !FAnimWeight::IsRelevant(FootState.IkAmount * FootState.LockAmount))
	{
		return;
	}

	const auto& ComponentTransform{GetProxyOnAnyThread<FAnimInstanceProxy>().GetComponentTransform()};

	FootState.LockLocation = ComponentTransform.TransformPosition(FootState.LockComponentRelativeLocation);
	FootState.LockRotation = ComponentTransform.TransformRotation(FootState.LockComponentRelativeRotation);

	if (MovementBase.bHasRelativeLocation)
	{
		const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

		FootState.LockMovementBaseRelativeLocation = BaseRotationInverse.RotateVector(FootState.LockLocation - MovementBase.Location);
		FootState.LockMovementBaseRelativeRotation = BaseRotationInverse * FootState.LockRotation;
	}
}

void UAlsAnimationInstance::ProcessFootLockBaseChange(FAlsFootState& FootState, const FTransform& ComponentTransformInverse) const
{
	if ((!bPendingUpdate && !MovementBase.bBaseChanged) ||
	    !FAnimWeight::IsRelevant(FootState.IkAmount * FootState.LockAmount))
	{
		return;
	}

	if (bPendingUpdate)
	{
		FootState.LockLocation = FootState.TargetLocation;
		FootState.LockRotation = FootState.TargetRotation;
	}

	FootState.LockComponentRelativeLocation = ComponentTransformInverse.TransformPosition(FootState.LockLocation);
	FootState.LockComponentRelativeRotation = ComponentTransformInverse.TransformRotation(FootState.LockRotation);

	if (MovementBase.bHasRelativeLocation)
	{
		const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

		FootState.LockMovementBaseRelativeLocation = BaseRotationInverse.RotateVector(FootState.LockLocation - MovementBase.Location);
		FootState.LockMovementBaseRelativeRotation = BaseRotationInverse * FootState.LockRotation;
	}
	else
	{
		FootState.LockMovementBaseRelativeLocation = FVector::ZeroVector;
		FootState.LockMovementBaseRelativeRotation = FQuat::Identity;
	}
}

void UAlsAnimationInstance::RefreshFootLock(FAlsFootState& FootState, const FName& FootLockCurveName,
                                            const FTransform& ComponentTransformInverse, const float DeltaTime,
                                            FVector& FinalLocation, FQuat& FinalRotation) const
{
	auto NewFootLockAmount{GetCurveValueClamped01(FootLockCurveName)};

	if (LocomotionState.bMovingSmooth || LocomotionMode != AlsLocomotionModeTags::Grounded)
	{
		// Smoothly disable foot locking if the character is moving or in the air,
		// instead of relying on the curve value from the animation blueprint.

		static constexpr auto MovingDecreaseSpeed{5.0f};
		static constexpr auto NotGroundedDecreaseSpeed{0.6f};

		NewFootLockAmount = bPendingUpdate
			                    ? 0.0f
			                    : FMath::Max(0.0f, FMath::Min(
				                                 NewFootLockAmount,
				                                 FootState.LockAmount - DeltaTime *
				                                 (LocomotionState.bMovingSmooth ? MovingDecreaseSpeed : NotGroundedDecreaseSpeed)));
	}

	if (Settings->Feet.bDisableFootLock || !FAnimWeight::IsRelevant(FootState.IkAmount * NewFootLockAmount))
	{
		if (FootState.LockAmount > 0.0f)
		{
			FootState.LockAmount = 0.0f;

			FootState.LockLocation = FVector::ZeroVector;
			FootState.LockRotation = FQuat::Identity;

			FootState.LockComponentRelativeLocation = FVector::ZeroVector;
			FootState.LockComponentRelativeRotation = FQuat::Identity;

			FootState.LockMovementBaseRelativeLocation = FVector::ZeroVector;
			FootState.LockMovementBaseRelativeRotation = FQuat::Identity;
		}

		return;
	}

	const auto bNewAmountEqualOne{FAnimWeight::IsFullWeight(NewFootLockAmount)};
	const auto bNewAmountGreaterThanPrevious{NewFootLockAmount > FootState.LockAmount};

	// Update the foot lock amount only if the new amount is less than the current amount or equal to 1. This
	// allows the foot to blend out from a locked location or lock to a new location, but never blend in.

	if (bNewAmountEqualOne)
	{
		if (bNewAmountGreaterThanPrevious)
		{
			// If the new foot lock amount is 1 and the previous amount is less than 1, then save the new foot lock location and rotation.

			if (FootState.LockAmount <= 0.9f)
			{
				// Keep the same lock location and rotation when the previous lock
				// amount is close to 1 to get rid of the foot "teleportation" issue.

				FootState.LockLocation = FinalLocation;
				FootState.LockRotation = FinalRotation;

				FootState.LockComponentRelativeLocation = ComponentTransformInverse.TransformPosition(FootState.LockLocation);
				FootState.LockComponentRelativeRotation = ComponentTransformInverse.TransformRotation(FootState.LockRotation);
			}

			if (MovementBase.bHasRelativeLocation)
			{
				const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

				FootState.LockMovementBaseRelativeLocation = BaseRotationInverse.RotateVector(FinalLocation - MovementBase.Location);
				FootState.LockMovementBaseRelativeRotation = BaseRotationInverse * FinalRotation;
			}
			else
			{
				FootState.LockMovementBaseRelativeLocation = FVector::ZeroVector;
				FootState.LockMovementBaseRelativeRotation = FQuat::Identity;
			}
		}

		FootState.LockAmount = 1.0f;
	}
	else if (!bNewAmountGreaterThanPrevious)
	{
		FootState.LockAmount = NewFootLockAmount;
	}

	if (FeetState.bInhibitFootLockForOneFrame)
	{
		// Inhibition is implemented by temporarily performing all calculations in component space rather
		// than in world space. So, the feet will still remain locked, but this time relative to the character.

		const auto& ComponentTransform{GetProxyOnAnyThread<FAnimInstanceProxy>().GetComponentTransform()};

		FootState.LockLocation = ComponentTransform.TransformPosition(FootState.LockComponentRelativeLocation);
		FootState.LockRotation = ComponentTransform.TransformRotation(FootState.LockComponentRelativeRotation);

		if (MovementBase.bHasRelativeLocation)
		{
			const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

			FootState.LockMovementBaseRelativeLocation = BaseRotationInverse.RotateVector(FootState.LockLocation - MovementBase.Location);
			FootState.LockMovementBaseRelativeRotation = BaseRotationInverse * FootState.LockRotation;
		}
	}
	else
	{
		if (MovementBase.bHasRelativeLocation)
		{
			FootState.LockLocation = MovementBase.Location + MovementBase.Rotation.RotateVector(FootState.LockMovementBaseRelativeLocation);
			FootState.LockRotation = MovementBase.Rotation * FootState.LockMovementBaseRelativeRotation;
		}

		FootState.LockComponentRelativeLocation = ComponentTransformInverse.TransformPosition(FootState.LockLocation);
		FootState.LockComponentRelativeRotation = ComponentTransformInverse.TransformRotation(FootState.LockRotation);
	}

	FinalLocation = FMath::Lerp(FinalLocation, FootState.LockLocation, FootState.LockAmount);
	FinalRotation = FQuat::Slerp(FinalRotation, FootState.LockRotation, FootState.LockAmount);
}

void UAlsAnimationInstance::RefreshFootOffset(FAlsFootState& FootState, const float DeltaTime,
                                              FVector& FinalLocation, FQuat& FinalRotation) const
{
	if (!FAnimWeight::IsRelevant(FootState.IkAmount))
	{
		FootState.OffsetTargetLocationZ = 0.0f;
		FootState.OffsetTargetRotation = FQuat::Identity;
		FootState.OffsetSpringState.Reset();
		return;
	}

	if (LocomotionMode == AlsLocomotionModeTags::InAir)
	{
		FootState.OffsetTargetLocationZ = 0.0f;
		FootState.OffsetTargetRotation = FQuat::Identity;
		FootState.OffsetSpringState.Reset();

		if (bPendingUpdate)
		{
			FootState.OffsetLocationZ = 0.0f;
			FootState.OffsetRotation = FQuat::Identity;
		}
		else
		{
			static constexpr auto InterpolationSpeed{15.0f};

			FootState.OffsetLocationZ = FMath::FInterpTo(FootState.OffsetLocationZ, 0.0f, DeltaTime, InterpolationSpeed);
			FootState.OffsetRotation = FMath::QInterpTo(FootState.OffsetRotation, FQuat::Identity, DeltaTime, InterpolationSpeed);

			FinalLocation.Z += FootState.OffsetLocationZ;
			FinalRotation = FootState.OffsetRotation * FinalRotation;
		}

		return;
	}

	// Trace downward from the foot location to find the geometry. If the surface is walkable, save the impact location and normal.

	const FVector TraceLocation{
		FinalLocation.X, FinalLocation.Y, GetProxyOnAnyThread<FAnimInstanceProxy>().GetComponentTransform().GetLocation().Z
	};

	FHitResult Hit;
	GetWorld()->LineTraceSingleByChannel(Hit,
	                                     TraceLocation + FVector{
		                                     0.0f, 0.0f, Settings->Feet.IkTraceDistanceUpward * LocomotionState.Scale
	                                     },
	                                     TraceLocation - FVector{
		                                     0.0f, 0.0f, Settings->Feet.IkTraceDistanceDownward * LocomotionState.Scale
	                                     },
	                                     Settings->Feet.IkTraceChannel, {__FUNCTION__, true, Character});

	const auto bGroundValid{Hit.IsValidBlockingHit() && Hit.ImpactNormal.Z >= LocomotionState.WalkableFloorZ};

#if WITH_EDITORONLY_DATA && ENABLE_DRAW_DEBUG
	if (bDisplayDebugTraces)
	{
		if (IsInGameThread())
		{
			UAlsUtility::DrawDebugLineTraceSingle(GetWorld(), Hit.TraceStart, Hit.TraceEnd, bGroundValid,
			                                      Hit, {0.0f, 0.25f, 1.0f}, {0.0f, 0.75f, 1.0f});
		}
		else
		{
			DisplayDebugTracesQueue.Emplace([this, Hit, bGroundValid]
				{
					UAlsUtility::DrawDebugLineTraceSingle(GetWorld(), Hit.TraceStart, Hit.TraceEnd, bGroundValid,
					                                      Hit, {0.0f, 0.25f, 1.0f}, {0.0f, 0.75f, 1.0f});
				}
			);
		}
	}
#endif

	if (bGroundValid)
	{
		const auto SlopeAngleCos{UE_REAL_TO_FLOAT(Hit.ImpactNormal.Z)};

		const auto FootHeight{Settings->Feet.FootHeight * LocomotionState.Scale};
		const auto FootHeightOffset{SlopeAngleCos > UE_SMALL_NUMBER ? FootHeight / SlopeAngleCos - FootHeight : 0.0f};

		// Find the difference between the impact location and the expected (flat) floor location.
		// These values are offset by the foot height to get better behavior on sloped surfaces.

		FootState.OffsetTargetLocationZ = UE_REAL_TO_FLOAT(Hit.ImpactPoint.Z - TraceLocation.Z) + FootHeightOffset;

		// Calculate the rotation offset.

		FootState.OffsetTargetRotation = FQuat::FindBetweenNormals(FVector::UpVector, Hit.ImpactNormal);
	}

	// Interpolate current offsets to the new target values.

	if (bPendingUpdate)
	{
		FootState.OffsetSpringState.Reset();

		FootState.OffsetLocationZ = FootState.OffsetTargetLocationZ;
		FootState.OffsetRotation = FootState.OffsetTargetRotation;
	}
	else
	{
		static constexpr auto LocationInterpolationFrequency{0.4f};
		static constexpr auto LocationInterpolationDampingRatio{4.0f};
		static constexpr auto LocationInterpolationTargetVelocityAmount{1.0f};

		FootState.OffsetLocationZ = UAlsMath::SpringDampFloat(FootState.OffsetLocationZ, FootState.OffsetTargetLocationZ,
		                                                      FootState.OffsetSpringState, DeltaTime, LocationInterpolationFrequency,
		                                                      LocationInterpolationDampingRatio, LocationInterpolationTargetVelocityAmount);

		static constexpr auto RotationInterpolationSpeed{30.0f};

		FootState.OffsetRotation = FMath::QInterpTo(FootState.OffsetRotation, FootState.OffsetTargetRotation,
		                                            DeltaTime, RotationInterpolationSpeed);
	}

	FinalLocation.Z += FootState.OffsetLocationZ;
	FinalRotation = FootState.OffsetRotation * FinalRotation;
}

void UAlsAnimationInstance::LimitFootRotation(const FAlsFootLimitsSettings& LimitsSettings,
                                              const FQuat& ParentRotation, FQuat& Rotation) const
{
	const auto RelativeRotation{ParentRotation.Inverse() * Rotation};

	FQuat Swing;
	FQuat Twist;
	RelativeRotation.ToSwingTwist(FVector{LimitsSettings.TwistAxis}, Swing, Twist);

	// Limit swing.

	const auto SwingLimitOffset{FQuat{LimitsSettings.SwingLimitOffsetQuaternion}};

	Swing = SwingLimitOffset * Swing;

	// Clamp a point with Swing.Y and Swing.Z coordinates to an ellipse with LimitsSettings.Swing2Limit
	// and LimitsSettings.Swing1Limit dimensions. A simplified and not very accurate algorithm is used here,
	// but it is enough for our needs. To get a more accurate result, you can use an algorithm similar
	// to the one used in Chaos::NearPointOnEllipse() or FRigUnit_SphericalPoseReader::DistanceToEllipse().

	FVector2D SwingLimit{Swing.Y, Swing.Z};
	SwingLimit.Normalize();

	SwingLimit.X = FMath::Abs(SwingLimit.X * LimitsSettings.Swing2Limit);
	SwingLimit.Y = FMath::Abs(SwingLimit.Y * LimitsSettings.Swing1Limit);

	const auto NewSwingY{FMath::Sign(Swing.Y) * FMath::Min(FMath::Abs(Swing.Y), SwingLimit.X)};
	const auto NewSwingZ{FMath::Sign(Swing.Z) * FMath::Min(FMath::Abs(Swing.Z), SwingLimit.Y)};

	FQuat NewSwing{
		0.0f, NewSwingY, NewSwingZ, FMath::Sqrt(FMath::Max(0.0f, 1.0f - NewSwingY * NewSwingY - NewSwingZ * NewSwingZ))
	};

	NewSwing = SwingLimitOffset.Inverse() * NewSwing;

	// Limit twist.

	const auto NewTwistX{FMath::Sign(Twist.X) * FMath::Min(FMath::Abs(Twist.X), LimitsSettings.TwistLimit)};

	const FQuat NewTwist(NewTwistX, 0.0f, 0.0f, FMath::Sqrt(FMath::Max(0.0f, 1.0f - NewTwistX * NewTwistX)));

	Rotation = ParentRotation * (NewSwing * NewTwist);
}

void UAlsAnimationInstance::PlayQuickStopAnimation()
{
	if (!IsValid(Settings))
	{
		return;
	}

	if (RotationMode != AlsRotationModeTags::VelocityDirection)
	{
		PlayTransitionLeftAnimation(Settings->Transitions.QuickStopBlendInDuration, Settings->Transitions.QuickStopBlendOutDuration,
		                            Settings->Transitions.QuickStopPlayRate.X, Settings->Transitions.QuickStopStartTime);
		return;
	}

	auto RotationYawAngle{
		FRotator3f::NormalizeAxis(UE_REAL_TO_FLOAT(
			(LocomotionState.bHasInput ? LocomotionState.InputYawAngle : LocomotionState.TargetYawAngle) - LocomotionState.Rotation.Yaw))
	};

	RotationYawAngle = UAlsMath::RemapAngleForCounterClockwiseRotation(RotationYawAngle);

	// Scale quick stop animation play rate based on how far the character
	// is going to rotate. At 180 degrees, the play rate will be maximal.

	if (RotationYawAngle <= 0.0f)
	{
		PlayTransitionLeftAnimation(Settings->Transitions.QuickStopBlendInDuration, Settings->Transitions.QuickStopBlendOutDuration,
		                            FMath::Lerp(Settings->Transitions.QuickStopPlayRate.X, Settings->Transitions.QuickStopPlayRate.Y,
		                                        FMath::Abs(RotationYawAngle) / 180.0f), Settings->Transitions.QuickStopStartTime);
	}
	else
	{
		PlayTransitionRightAnimation(Settings->Transitions.QuickStopBlendInDuration, Settings->Transitions.QuickStopBlendOutDuration,
		                             FMath::Lerp(Settings->Transitions.QuickStopPlayRate.X, Settings->Transitions.QuickStopPlayRate.Y,
		                                         FMath::Abs(RotationYawAngle) / 180.0f), Settings->Transitions.QuickStopStartTime);
	}
}

void UAlsAnimationInstance::PlayTransitionAnimation(UAnimSequenceBase* Animation, const float BlendInDuration, const float BlendOutDuration,
                                                    const float PlayRate, const float StartTime, const bool bFromStandingIdleOnly)
{
	if (bFromStandingIdleOnly && (LocomotionState.bMoving || Stance != AlsStanceTags::Standing))
	{
		return;
	}

	// Animation montages can't be played in the worker thread, so queue them up to play later in the game thread.

	TransitionsState.QueuedTransitionAnimation = Animation;
	TransitionsState.QueuedTransitionBlendInDuration = BlendInDuration;
	TransitionsState.QueuedTransitionBlendOutDuration = BlendOutDuration;
	TransitionsState.QueuedTransitionPlayRate = PlayRate;
	TransitionsState.QueuedTransitionStartTime = StartTime;

	if (IsInGameThread())
	{
		PlayQueuedTransitionAnimation();
	}
}

void UAlsAnimationInstance::PlayTransitionLeftAnimation(const float BlendInDuration, const float BlendOutDuration, const float PlayRate,
                                                        const float StartTime, const bool bFromStandingIdleOnly)
{
	if (!IsValid(Settings))
	{
		return;
	}

	PlayTransitionAnimation(Stance == AlsStanceTags::Crouching
		                        ? Settings->Transitions.CrouchingLeftAnimation
		                        : Settings->Transitions.StandingLeftAnimation,
	                        BlendInDuration, BlendOutDuration, PlayRate, StartTime, bFromStandingIdleOnly);
}

void UAlsAnimationInstance::PlayTransitionRightAnimation(const float BlendInDuration, const float BlendOutDuration, const float PlayRate,
                                                         const float StartTime, const bool bFromStandingIdleOnly)
{
	if (!IsValid(Settings))
	{
		return;
	}

	PlayTransitionAnimation(Stance == AlsStanceTags::Crouching
		                        ? Settings->Transitions.CrouchingRightAnimation
		                        : Settings->Transitions.StandingRightAnimation,
	                        BlendInDuration, BlendOutDuration, PlayRate, StartTime, bFromStandingIdleOnly);
}

void UAlsAnimationInstance::StopTransitionAndTurnInPlaceAnimations(const float BlendOutDuration)
{
	TransitionsState.bStopTransitionsQueued = true;
	TransitionsState.QueuedStopTransitionsBlendOutDuration = BlendOutDuration;

	if (IsInGameThread())
	{
		StopQueuedTransitionAndTurnInPlaceAnimations();
	}
}

void UAlsAnimationInstance::RefreshTransitions()
{
	// The allow transitions curve is modified within certain states, so that transitions allowed will be true while in those states.

	TransitionsState.bTransitionsAllowed = FAnimWeight::IsFullWeight(GetCurveValue(UAlsConstants::AllowTransitionsCurveName()));
}

void UAlsAnimationInstance::RefreshDynamicTransitions()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshDynamicTransitions()"),
	                            STAT_UAlsAnimationInstance_RefreshDynamicTransitions, STATGROUP_Als)

	if (DynamicTransitionsState.bUpdatedThisFrame || !IsValid(Settings))
	{
		return;
	}

	DynamicTransitionsState.bUpdatedThisFrame = true;

	if (DynamicTransitionsState.FrameDelay > 0)
	{
		DynamicTransitionsState.FrameDelay -= 1;
		return;
	}

	if (!TransitionsState.bTransitionsAllowed)
	{
		return;
	}

	// Check each foot to see if the location difference between the foot look and its desired / target location
	// exceeds a threshold. If it does, play an additive transition animation on that foot. The currently set
	// transition plays the second half of a 2 foot transition animation, so that only a single foot moves.

	const auto FootLockDistanceThresholdSquared{
		FMath::Square(Settings->DynamicTransitions.FootLockDistanceThreshold * LocomotionState.Scale)
	};

	const auto FootLockLeftDistanceSquared{FVector::DistSquared(FeetState.Left.TargetLocation, FeetState.Left.LockLocation)};
	const auto FootLockRightDistanceSquared{FVector::DistSquared(FeetState.Right.TargetLocation, FeetState.Right.LockLocation)};

	const auto bTransitionLeftAllowed{
		FAnimWeight::IsRelevant(FeetState.Left.LockAmount) && FootLockLeftDistanceSquared > FootLockDistanceThresholdSquared
	};

	const auto bTransitionRightAllowed{
		FAnimWeight::IsRelevant(FeetState.Right.LockAmount) && FootLockRightDistanceSquared > FootLockDistanceThresholdSquared
	};

	if (!bTransitionLeftAllowed && !bTransitionRightAllowed)
	{
		return;
	}

	TObjectPtr<UAnimSequenceBase> DynamicTransitionAnimation;

	// If both transitions are allowed, choose the one with a greater lock distance.

	if (!bTransitionLeftAllowed)
	{
		DynamicTransitionAnimation = Stance == AlsStanceTags::Crouching
			                             ? Settings->DynamicTransitions.CrouchingRightAnimation
			                             : Settings->DynamicTransitions.StandingRightAnimation;
	}
	else if (!bTransitionRightAllowed)
	{
		DynamicTransitionAnimation = Stance == AlsStanceTags::Crouching
			                             ? Settings->DynamicTransitions.CrouchingLeftAnimation
			                             : Settings->DynamicTransitions.StandingLeftAnimation;
	}
	else if (FootLockLeftDistanceSquared >= FootLockRightDistanceSquared)
	{
		DynamicTransitionAnimation = Stance == AlsStanceTags::Crouching
			                             ? Settings->DynamicTransitions.CrouchingLeftAnimation
			                             : Settings->DynamicTransitions.StandingLeftAnimation;
	}
	else
	{
		DynamicTransitionAnimation = Stance == AlsStanceTags::Crouching
			                             ? Settings->DynamicTransitions.CrouchingRightAnimation
			                             : Settings->DynamicTransitions.StandingRightAnimation;
	}

	if (IsValid(DynamicTransitionAnimation))
	{
		// Block next dynamic transitions for about 2 frames to give the animation blueprint some time to properly react to the animation.

		DynamicTransitionsState.FrameDelay = 2;

		// Animation montages can't be played in the worker thread, so queue them up to play later in the game thread.

		TransitionsState.QueuedTransitionAnimation = DynamicTransitionAnimation;
		TransitionsState.QueuedTransitionBlendInDuration = Settings->DynamicTransitions.BlendDuration;
		TransitionsState.QueuedTransitionBlendOutDuration = Settings->DynamicTransitions.BlendDuration;
		TransitionsState.QueuedTransitionPlayRate = Settings->DynamicTransitions.PlayRate;
		TransitionsState.QueuedTransitionStartTime = 0.0f;

		if (IsInGameThread())
		{
			PlayQueuedTransitionAnimation();
		}
	}
}

void UAlsAnimationInstance::PlayQueuedTransitionAnimation()
{
	check(IsInGameThread())

	if (TransitionsState.bStopTransitionsQueued || !IsValid(TransitionsState.QueuedTransitionAnimation))
	{
		return;
	}

	PlaySlotAnimationAsDynamicMontage(TransitionsState.QueuedTransitionAnimation, UAlsConstants::TransitionSlotName(),
	                                  TransitionsState.QueuedTransitionBlendInDuration, TransitionsState.QueuedTransitionBlendOutDuration,
	                                  TransitionsState.QueuedTransitionPlayRate, 1, 0.0f, TransitionsState.QueuedTransitionStartTime);

	TransitionsState.QueuedTransitionAnimation = nullptr;
	TransitionsState.QueuedTransitionBlendInDuration = 0.0f;
	TransitionsState.QueuedTransitionBlendOutDuration = 0.0f;
	TransitionsState.QueuedTransitionPlayRate = 1.0f;
	TransitionsState.QueuedTransitionStartTime = 0.0f;
}

void UAlsAnimationInstance::StopQueuedTransitionAndTurnInPlaceAnimations()
{
	check(IsInGameThread())

	if (!TransitionsState.bStopTransitionsQueued)
	{
		return;
	}

	StopSlotAnimation(TransitionsState.QueuedStopTransitionsBlendOutDuration, UAlsConstants::TransitionSlotName());
	StopSlotAnimation(TransitionsState.QueuedStopTransitionsBlendOutDuration, UAlsConstants::TurnInPlaceStandingSlotName());
	StopSlotAnimation(TransitionsState.QueuedStopTransitionsBlendOutDuration, UAlsConstants::TurnInPlaceCrouchingSlotName());

	TransitionsState.bStopTransitionsQueued = false;
	TransitionsState.QueuedStopTransitionsBlendOutDuration = 0.0f;
}

bool UAlsAnimationInstance::IsRotateInPlaceAllowed()
{
	return RotationMode == AlsRotationModeTags::Aiming || ViewMode == AlsViewModeTags::FirstPerson;
}

void UAlsAnimationInstance::RefreshRotateInPlace()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshRotateInPlace()"),
	                            STAT_UAlsAnimationInstance_RefreshRotateInPlace, STATGROUP_Als)

	if (RotateInPlaceState.bUpdatedThisFrame || !IsValid(Settings))
	{
		return;
	}

	RotateInPlaceState.bUpdatedThisFrame = true;

	if (LocomotionState.bMoving || !IsRotateInPlaceAllowed())
	{
		RotateInPlaceState.bRotatingLeft = false;
		RotateInPlaceState.bRotatingRight = false;
	}
	else
	{
		// Check if the character should rotate left or right by checking if the view yaw angle exceeds the threshold.

		RotateInPlaceState.bRotatingLeft = ViewState.YawAngle < -Settings->RotateInPlace.ViewYawAngleThreshold;
		RotateInPlaceState.bRotatingRight = ViewState.YawAngle > Settings->RotateInPlace.ViewYawAngleThreshold;
	}

	static constexpr auto PlayRateInterpolationSpeed{5.0f};

	if (!RotateInPlaceState.bRotatingLeft && !RotateInPlaceState.bRotatingRight)
	{
		RotateInPlaceState.PlayRate = bPendingUpdate
			                              ? Settings->RotateInPlace.PlayRate.X
			                              : FMath::FInterpTo(RotateInPlaceState.PlayRate, Settings->RotateInPlace.PlayRate.X,
			                                                 GetDeltaSeconds(), PlayRateInterpolationSpeed);
		return;
	}

	// If the character should rotate, set the play rate to scale with the view yaw
	// speed. This makes the character rotate faster when moving the camera faster.

	const auto PlayRate{
		FMath::GetMappedRangeValueClamped(Settings->RotateInPlace.ReferenceViewYawSpeed,
		                                  Settings->RotateInPlace.PlayRate, ViewState.YawSpeed)
	};

	RotateInPlaceState.PlayRate = bPendingUpdate
		                              ? PlayRate
		                              : FMath::FInterpTo(RotateInPlaceState.PlayRate, PlayRate,
		                                                 GetDeltaSeconds(), PlayRateInterpolationSpeed);

	if (ViewState.YawSpeed > Settings->RotateInPlace.FootLockInhibitionViewYawSpeedThreshold ||
	    FMath::Abs(ViewState.YawAngle) > Settings->RotateInPlace.FootLockInhibitionViewYawAngleThreshold)
	{
		// Inhibit foot locking when rotating at a large angle or rotating too fast, otherwise the legs may twist into a spiral.
		InhibitFootLockForOneFrame();
	}
}

bool UAlsAnimationInstance::IsTurnInPlaceAllowed()
{
	return RotationMode == AlsRotationModeTags::ViewDirection && ViewMode != AlsViewModeTags::FirstPerson;
}

void UAlsAnimationInstance::InitializeTurnInPlace()
{
	TurnInPlaceState.ActivationDelay = 0.0f;
}

void UAlsAnimationInstance::RefreshTurnInPlace()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshTurnInPlace()"),
	                            STAT_UAlsAnimationInstance_RefreshTurnInPlace, STATGROUP_Als)

	if (TurnInPlaceState.bUpdatedThisFrame || !IsValid(Settings))
	{
		return;
	}

	TurnInPlaceState.bUpdatedThisFrame = true;

	if (!TransitionsState.bTransitionsAllowed || !IsTurnInPlaceAllowed())
	{
		TurnInPlaceState.ActivationDelay = 0.0f;
		return;
	}

	// Check if the view yaw speed is below the threshold and if the view yaw angle is outside the
	// threshold. If so, begin counting the activation delay time. If not, reset the activation delay
	// time. This ensures the conditions remain true for a sustained time before turning in place.

	if (ViewState.YawSpeed >= Settings->TurnInPlace.ViewYawSpeedThreshold ||
	    FMath::Abs(ViewState.YawAngle) <= Settings->TurnInPlace.ViewYawAngleThreshold)
	{
		TurnInPlaceState.ActivationDelay = 0.0f;
		return;
	}

	TurnInPlaceState.ActivationDelay = TurnInPlaceState.ActivationDelay + GetDeltaSeconds();

	const auto ActivationDelay{
		FMath::GetMappedRangeValueClamped({Settings->TurnInPlace.ViewYawAngleThreshold, 180.0f},
		                                  Settings->TurnInPlace.ViewYawAngleToActivationDelay,
		                                  FMath::Abs(ViewState.YawAngle))
	};

	// Check if the activation delay time exceeds the set delay (mapped to the view yaw angle). If so, start a turn in place.

	if (TurnInPlaceState.ActivationDelay <= ActivationDelay)
	{
		return;
	}

	// Select settings based on turn angle and stance.

	const auto bTurnLeft{
		ViewState.YawAngle <= 0.0f ||
		ViewState.YawAngle > 180.0f - UAlsMath::CounterClockwiseRotationAngleThreshold
	};

	UAlsTurnInPlaceSettings* TurnInPlaceSettings{nullptr};
	FName TurnInPlaceSlotName;

	if (Stance == AlsStanceTags::Standing)
	{
		TurnInPlaceSlotName = UAlsConstants::TurnInPlaceStandingSlotName();

		if (FMath::Abs(ViewState.YawAngle) < Settings->TurnInPlace.Turn180AngleThreshold)
		{
			TurnInPlaceSettings = bTurnLeft
				                      ? Settings->TurnInPlace.StandingTurn90Left
				                      : Settings->TurnInPlace.StandingTurn90Right;
		}
		else
		{
			TurnInPlaceSettings = bTurnLeft
				                      ? Settings->TurnInPlace.StandingTurn180Left
				                      : Settings->TurnInPlace.StandingTurn180Right;
		}
	}
	else if (Stance == AlsStanceTags::Crouching)
	{
		TurnInPlaceSlotName = UAlsConstants::TurnInPlaceCrouchingSlotName();

		if (FMath::Abs(ViewState.YawAngle) < Settings->TurnInPlace.Turn180AngleThreshold)
		{
			TurnInPlaceSettings = bTurnLeft
				                      ? Settings->TurnInPlace.CrouchingTurn90Left
				                      : Settings->TurnInPlace.CrouchingTurn90Right;
		}
		else
		{
			TurnInPlaceSettings = bTurnLeft
				                      ? Settings->TurnInPlace.CrouchingTurn180Left
				                      : Settings->TurnInPlace.CrouchingTurn180Right;
		}
	}

	if (IsValid(TurnInPlaceSettings) && ALS_ENSURE(IsValid(TurnInPlaceSettings->Animation)))
	{
		// Animation montages can't be played in the worker thread, so queue them up to play later in the game thread.

		TurnInPlaceState.QueuedSettings = TurnInPlaceSettings;
		TurnInPlaceState.QueuedSlotName = TurnInPlaceSlotName;
		TurnInPlaceState.QueuedTurnYawAngle = ViewState.YawAngle;

		if (IsInGameThread())
		{
			PlayQueuedTurnInPlaceAnimation();
		}
	}
}

void UAlsAnimationInstance::PlayQueuedTurnInPlaceAnimation()
{
	check(IsInGameThread())

	if (TransitionsState.bStopTransitionsQueued || !IsValid(TurnInPlaceState.QueuedSettings))
	{
		return;
	}

	const auto* TurnInPlaceSettings{TurnInPlaceState.QueuedSettings.Get()};

	PlaySlotAnimationAsDynamicMontage(TurnInPlaceSettings->Animation, TurnInPlaceState.QueuedSlotName,
	                                  Settings->TurnInPlace.BlendDuration, Settings->TurnInPlace.BlendDuration,
	                                  TurnInPlaceSettings->PlayRate, 1, 0.0f);

	// Scale the rotation yaw delta (gets scaled in animation graph) to compensate for play rate and turn angle (if allowed).

	TurnInPlaceState.PlayRate = TurnInPlaceSettings->PlayRate;

	if (TurnInPlaceSettings->bScalePlayRateByAnimatedTurnAngle)
	{
		TurnInPlaceState.PlayRate *= FMath::Abs(TurnInPlaceState.QueuedTurnYawAngle / TurnInPlaceSettings->AnimatedTurnAngle);
	}

	TurnInPlaceState.QueuedSettings = nullptr;
	TurnInPlaceState.QueuedSlotName = NAME_None;
	TurnInPlaceState.QueuedTurnYawAngle = 0.0f;
}

void UAlsAnimationInstance::RefreshRagdollingOnGameThread()
{
	check(IsInGameThread())

	if (LocomotionAction != AlsLocomotionActionTags::Ragdolling)
	{
		return;
	}

	// Scale the flail play rate by the root speed. The faster the ragdoll moves, the faster the character will flail.

	static constexpr auto ReferenceSpeed{1000.0f};

	RagdollingState.FlailPlayRate = UAlsMath::Clamp01(UE_REAL_TO_FLOAT(Character->GetRagdollingState().Velocity.Size() / ReferenceSpeed));
}

FPoseSnapshot& UAlsAnimationInstance::SnapshotFinalRagdollPose()
{
	check(IsInGameThread())

	// Save a snapshot of the current ragdoll pose for use in animation graph to blend out of the ragdoll.

	SnapshotPose(RagdollingState.FinalRagdollPose);

	return RagdollingState.FinalRagdollPose;
}

float UAlsAnimationInstance::GetCurveValueClamped01(const FName& CurveName) const
{
	return UAlsMath::Clamp01(GetCurveValue(CurveName));
}
