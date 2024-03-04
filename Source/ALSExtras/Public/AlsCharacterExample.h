#pragma once

#include "AlsCharacter.h"
#include "AlsCharacterExample.generated.h"

struct FInputActionValue;
class UAlsCameraComponent;
class UInputMappingContext;
class UInputAction;

UCLASS(AutoExpandCategories = ("Settings|Als Character Example", "State|Als Character Example"))
class ALSEXTRAS_API AAlsCharacterExample : public AAlsCharacter
{
	GENERATED_BODY()

protected:
	UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "Als Character Example")
	TObjectPtr<UAlsCameraComponent> Camera;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputMappingContext> InputMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> LookMouseAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> SprintAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> WalkAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> CrouchAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> AimAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> RagdollAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> RollAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> RotationModeAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> ViewModeAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Settings|Als Character Example", Meta = (DisplayThumbnail = false))
	TObjectPtr<UInputAction> SwitchShoulderAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings|Als Character Example", Meta = (ClampMin = 0, ForceUnits = "x"))
	float LookUpMouseSensitivity{1.0f};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings|Als Character Example", Meta = (ClampMin = 0, ForceUnits = "x"))
	float LookRightMouseSensitivity{1.0f};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings|Als Character Example",
		Meta = (ClampMin = 0, ForceUnits = "deg/s"))
	float LookUpRate{90.0f};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings|Als Character Example",
		Meta = (ClampMin = 0, ForceUnits = "deg/s"))
	float LookRightRate{240.0f};

public:
	AAlsCharacterExample();

	virtual void NotifyControllerChanged() override;

	// Camera

protected:
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& ViewInfo) override;

	// Input

protected:
	virtual void SetupPlayerInputComponent(UInputComponent* Input) override;

private:

	
	//void Input_OnLookMouse(const FInputActionValue& ActionValue);
	//Mouse Input Action
	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnLookMouse(const FVector2D& Value);

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnLook(const FVector2D& Value);

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnMove(const FVector2D& Value);

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnSprint(bool Value);

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnWalk();

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnCrouch();

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnJump(bool Value);

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnAim(bool Value);

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnRagdoll();

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnRoll();

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnRotationMode();

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnViewMode();

	UFUNCTION(BlueprintCallable, Category = "Als|Input")
	void Input_OnSwitchShoulder();

	// Debug

public:
	virtual void DisplayDebug(UCanvas* Canvas, const FDebugDisplayInfo& DisplayInfo, float& Unused, float& VerticalLocation) override;

	//Use to override OnFellOutOfWorld
	void FellOutOfWorld(const class UDamageType& dmgType) override;

	//When character fall out of the world, call this in blueprint
	UFUNCTION(BlueprintImplementableEvent, Category = "Als|Gameplay")
	void OnFellOutOfWorld();
};
