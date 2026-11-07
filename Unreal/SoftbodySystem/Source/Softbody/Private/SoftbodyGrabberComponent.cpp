// Copyright (c) 2026 wjsekrud

#include "SoftbodyGrabberComponent.h"

#include "Components/MeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "SoftbodyComponent.h"
#include "SoftbodySurfaceField.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoftbodyGrabber, Log, All);

USoftbodyGrabberComponent::USoftbodyGrabberComponent()
{
	// 끄는 자리는 Softbody 컴포넌트가 틱마다 이 컴포넌트 자리에서 읽는다 — 틱이 필요 없다.
	PrimaryComponentTick.bCanEverTick = false;
}

bool USoftbodyGrabberComponent::GrabAtLocation(FVector Location, USoftbodyComponent* Target)
{
	if (!Target)
	{
		Target = FindSoftbodyNear(GetWorld(), Location, nullptr);
	}
	return BeginOn(Target, Location, FVector::ZeroVector);
}

bool USoftbodyGrabberComponent::GrabAtLocationWithin(const FVector& Location, USoftbodyComponent* Target, float SearchDistance)
{
	if (!Target)
	{
		Target = FindSoftbodyNear(GetWorld(), Location, nullptr);
	}
	return BeginOn(Target, Location, FVector::ZeroVector, SearchDistance);
}

bool USoftbodyGrabberComponent::GrabAtHit(const FHitResult& Hit)
{
	const FVector Location = Hit.bBlockingHit || Hit.bStartPenetrating ? FVector(Hit.ImpactPoint) : FVector(Hit.Location);
	const UPrimitiveComponent* HitComponent = Hit.GetComponent();
	USoftbodyComponent* Target = nullptr;
	// 맞은 메시를 변형하는 Softbody가 먼저(같은 액터에 메시가 여럿이면 그 메시의 것).
	if (const AActor* Actor = HitComponent ? HitComponent->GetOwner() : nullptr)
	{
		TInlineComponentArray<USoftbodyComponent*> Softbodies(Actor);
		for (USoftbodyComponent* Candidate : Softbodies)
		{
			if (Candidate && Candidate->GetTargetMeshComponent() == HitComponent)
			{
				Target = Candidate;
				break;
			}
		}
	}
	if (!Target)
	{
		Target = FindSoftbodyNear(GetWorld(), Location, HitComponent);
	}
	const FVector Direction = (FVector(Hit.TraceEnd) - FVector(Hit.TraceStart)).GetSafeNormal();
	return BeginOn(Target, Location, Direction);
}

bool USoftbodyGrabberComponent::BeginOn(USoftbodyComponent* Target, const FVector& Location, const FVector& TraceDirection, float SearchDistance)
{
	if (IsGrabbing())
	{
		Release();
	}
	if (!Target)
	{
		LastFailure = FString::Printf(TEXT("No Softbody mesh within %.0f cm."), SoftbodyGrab::SearchDistance);
		UE_LOG(LogSoftbodyGrabber, Log, TEXT("%s: nothing grabbed - no Softbody mesh within %.0f cm of %s."), *GetPathName(),
			SoftbodyGrab::SearchDistance, *Location.ToCompactString());
		return false;
	}
	FString Reason;
	uint32 Serial = 0;
	const int32 Slot = Target->BeginGrab(this, Location, TraceDirection, FMath::Max(GrabRadius, 0.1f), Serial, &Reason, SearchDistance);
	if (Slot == INDEX_NONE)
	{
		LastFailure = FString::Printf(TEXT("Nothing grabbed: %s."), *Reason);
		UE_LOG(LogSoftbodyGrabber, Log, TEXT("%s: nothing grabbed on %s - %s."), *GetPathName(), *Target->GetPathName(), *Reason);
		return false;
	}
	LastFailure.Reset();
	Grabbed = Target;
	GrabSlot = Slot;
	GrabSerial = Serial;
	return true;
}

void USoftbodyGrabberComponent::Release()
{
	USoftbodyComponent* Target = Grabbed.Get();
	const bool bWasGrabbing = IsGrabbing();
	if (Target && GrabSlot != INDEX_NONE)
	{
		Target->EndGrab(GrabSlot, GrabSerial);
	}
	Grabbed.Reset();
	GrabSlot = INDEX_NONE;
	GrabSerial = 0;
	if (bWasGrabbing)
	{
		OnGrabEnded.Broadcast(ESoftbodyGrabEndReason::Released);
	}
}

bool USoftbodyGrabberComponent::IsGrabbing() const
{
	return GrabSlot != INDEX_NONE && Grabbed.IsValid();
}

USoftbodyComponent* USoftbodyGrabberComponent::GetGrabbedSoftbody() const
{
	return IsGrabbing() ? Grabbed.Get() : nullptr;
}

bool USoftbodyGrabberComponent::GetGrabState(FGrabState& Out) const
{
	const USoftbodyComponent* Target = GetGrabbedSoftbody();
	USoftbodyComponent::FGrabInfo Info;
	if (!Target || !Target->GetGrabInfo(GrabSlot, GrabSerial, Info))
	{
		return false;
	}
	Out.Stretch = Info.Stretch;
	Out.MaxStretch = Info.Limit * SoftbodyGrab::MaxStretchRatio;
	Out.Flesh = Info.Flesh;
	Out.LumpFollow = Info.LumpFollow;
	Out.GrabbedPoint = Info.GrabbedPoint;
	Out.bHeld = Info.bHeld;
	return true;
}

bool USoftbodyGrabberComponent::GetStretch(float& Stretch, float& MaxStretch) const
{
	FGrabState State;
	if (!GetGrabState(State) || !State.bHeld)
	{
		Stretch = 0.f;
		MaxStretch = 0.f;
		return false;
	}
	Stretch = State.Stretch;
	MaxStretch = State.MaxStretch;
	return true;
}

void USoftbodyGrabberComponent::NotifyGrabEnded(ESoftbodyGrabEndReason Reason, uint32 Serial)
{
	if (Serial != GrabSerial || GrabSlot == INDEX_NONE)
	{
		return;
	}
	Grabbed.Reset();
	GrabSlot = INDEX_NONE;
	GrabSerial = 0;
	if (Reason == ESoftbodyGrabEndReason::TargetLost)
	{
		LastFailure = TEXT("The Softbody was turned off, lost its contact or was destroyed.");
	}
	if (Reason == ESoftbodyGrabEndReason::Missed)
	{
		LastFailure = TEXT("No skin to hold there: the Depth paint is black, it is a rigid plate, or the slot is not pressable.");
		UE_LOG(LogSoftbodyGrabber, Log,
			TEXT("%s: missed - no skin to hold within %.0f cm of the grab point (or the skin there is painted black in Depth, or is a rigid plate)."),
			*GetPathName(), SoftbodyGrab::SearchDistance);
	}
	OnGrabEnded.Broadcast(Reason);
}

void USoftbodyGrabberComponent::OnUnregister()
{
	// Grabber가 사라지면 잡기도 풀린다(남은 당김은 Recovery Rate로 돌아온다).
	if (IsGrabbing())
	{
		Release();
	}
	Super::OnUnregister();
}

USoftbodyComponent* USoftbodyGrabberComponent::FindSoftbodyNear(const UWorld* World, const FVector& Location, const UPrimitiveComponent* Preferred)
{
	if (!World)
	{
		return nullptr;
	}
	// 대상 메시 바운드를 찾는 거리만큼 키운 상자가 점을 품는 것 가운데 바운드가 작은 쪽(따로 된 머리 메시가 몸보다 먼저). 그 밖은 피부가 찾는 거리 밖이다.
	USoftbodyComponent* Inside = nullptr;
	double InsideVolume = TNumericLimits<double>::Max();
	for (TObjectIterator<USoftbodyComponent> It; It; ++It)
	{
		USoftbodyComponent* Candidate = *It;
		if (!Candidate || Candidate->GetWorld() != World || !Candidate->IsActive() || !Candidate->bEnableContact || !IsValid(Candidate))
		{
			continue;
		}
		const UMeshComponent* Mesh = Candidate->GetTargetMeshComponent();
		if (!Mesh)
		{
			continue;
		}
		if (Preferred && Mesh == Preferred)
		{
			return Candidate;
		}
		const FBox Box = Mesh->Bounds.GetBox();
		if (Box.ExpandBy((double)SoftbodyGrab::SearchDistance).IsInside(Location))
		{
			const double Volume = Box.GetVolume();
			if (Volume < InsideVolume)
			{
				InsideVolume = Volume;
				Inside = Candidate;
			}
		}
	}
	return Inside;
}
