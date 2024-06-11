﻿#pragma once

#include "AnimationModifier.h"
#include "Utility/AlsConstants.h"
#include "AlsAnimationModifier_CreateLayeringCurves.generated.h"

UCLASS(DisplayName = "Als Create Layering Curves Animation Modifier")
class ALSEDITOR_API UAlsAnimationModifier_CreateLayeringCurves : public UAnimationModifier
{
	GENERATED_BODY()

protected:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings")
	uint8 bOverrideExistingCurves : 1 {false};

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings")
	uint8 bAddKeyOnEachFrame : 1 {false};

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings")
	float CurveValue{0.0f};

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings")
	TArray<FName> CurveNames
	{
		UAlsConstants::LayerHeadCurveName(),
		UAlsConstants::LayerHeadAdditiveCurveName(),
		UAlsConstants::LayerArmLeftCurveName(),
		UAlsConstants::LayerArmLeftAdditiveCurveName(),
		UAlsConstants::LayerArmLeftLocalSpaceCurveName(),
		UAlsConstants::LayerArmRightCurveName(),
		UAlsConstants::LayerArmRightAdditiveCurveName(),
		UAlsConstants::LayerArmRightLocalSpaceCurveName(),
		UAlsConstants::LayerHandLeftCurveName(),
		UAlsConstants::LayerHandRightCurveName(),
		UAlsConstants::LayerSpineCurveName(),
		UAlsConstants::LayerSpineAdditiveCurveName(),
		UAlsConstants::LayerPelvisCurveName(),
		UAlsConstants::LayerLegsCurveName(),

		UAlsConstants::HandLeftIkCurveName(),
		UAlsConstants::HandRightIkCurveName(),

		UAlsConstants::ViewBlockCurveName(),
		UAlsConstants::AllowAimingCurveName(),

		UAlsConstants::HipsDirectionLockCurveName(),
	};

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings")
	uint8 bAddSlotCurves : 1 {false};

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings")
	float SlotCurveValue{1.0f};

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings")
	TArray<FName> SlotCurveNames
	{
		UAlsConstants::LayerHeadSlotCurveName(),
		UAlsConstants::LayerArmLeftSlotCurveName(),
		UAlsConstants::LayerArmRightSlotCurveName(),
		UAlsConstants::LayerSpineSlotCurveName(),
		UAlsConstants::LayerPelvisSlotCurveName(),
		UAlsConstants::LayerLegsSlotCurveName(),
	};

public:
	virtual void OnApply_Implementation(UAnimSequence* Sequence) override;

private:
	void CreateCurves(UAnimSequence* Sequence, const TArray<FName>& Names, float Value) const;
};
