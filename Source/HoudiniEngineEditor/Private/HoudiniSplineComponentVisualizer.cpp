/*
* Copyright (c) <2018> Side Effects Software Inc.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. The name of Side Effects Software may not be used to endorse or
*    promote products derived from this software without specific prior
*    written permission.
*
* THIS SOFTWARE IS PROVIDED BY SIDE EFFECTS SOFTWARE "AS IS" AND ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
* NO EVENT SHALL SIDE EFFECTS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "HoudiniSplineComponentVisualizer.h"

#include "HoudiniEngineEditor.h"
#include "HoudiniEngineEditorPrivatePCH.h"
#include "HoudiniApi.h"
#include "HoudiniAssetComponent.h"
#include "HoudiniSplineComponent.h"
#include "HoudiniInputObject.h"
#include "HoudiniInput.h"

#include "HoudiniEngineUtils.h"

#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "ComponentVisualizerManager.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ScopedTransaction.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "HModel.h"

#define LOCTEXT_NAMESPACE HOUDINI_LOCTEXT_NAMESPACE 

IMPLEMENT_HIT_PROXY(HHoudiniSplineVisProxy, HComponentVisProxy);
IMPLEMENT_HIT_PROXY(HHoudiniSplineControlPointVisProxy, HHoudiniSplineVisProxy);
IMPLEMENT_HIT_PROXY(HHoudiniSplineCurveSegmentVisProxy, HHoudiniSplineVisProxy);

HHoudiniSplineVisProxy::HHoudiniSplineVisProxy(const UActorComponent * InComponent)
	: HComponentVisProxy(InComponent, HPP_Wireframe)
{}

HHoudiniSplineControlPointVisProxy::HHoudiniSplineControlPointVisProxy(
	const UActorComponent * InComponent, int32 InControlPointIndex)
	: HHoudiniSplineVisProxy(InComponent)
	, ControlPointIndex(InControlPointIndex)
{}

HHoudiniSplineCurveSegmentVisProxy::HHoudiniSplineCurveSegmentVisProxy(
	const UActorComponent * InComponent, int32 InDisplayPointIndex)
	: HHoudiniSplineVisProxy(InComponent)
	, DisplayPointIndex(InDisplayPointIndex)
{}

FHoudiniSplineComponentVisualizerCommands::FHoudiniSplineComponentVisualizerCommands()
	: TCommands< FHoudiniSplineComponentVisualizerCommands >(
		"HoudiniSplineComponentVisualizer",
		LOCTEXT("HoudiniSplineComponentVisualizer", "Houdini Spline Component Visualizer"),
		NAME_None,
		FEditorStyle::GetStyleSetName())
{}

void FHoudiniSplineComponentVisualizerCommands::RegisterCommands()
{
	UI_COMMAND(
		CommandAddControlPoint, "Add Control Point", "Add control points.",
		EUserInterfaceActionType::Button, FInputChord());

	UI_COMMAND(
		CommandDuplicateControlPoint, "Duplicate Control Point", "Duplicate control points.",
		EUserInterfaceActionType::Button, FInputChord());

	UI_COMMAND(
		CommandDeleteControlPoint, "Delete Control Point", "delete control points.",
		EUserInterfaceActionType::Button, FInputChord(EKeys::Delete));

	UI_COMMAND(CommandDeselectAllControlPoints, "Deselect All", "Deselect all control points.",
		EUserInterfaceActionType::Button, FInputChord());

	UI_COMMAND(CommandInsertControlPoint, "Insert Control Point", "Insert a control point on curve.",
		EUserInterfaceActionType::Button, FInputChord());
}


FHoudiniSplineComponentVisualizer::FHoudiniSplineComponentVisualizer()
	:FComponentVisualizer()
	,bAllowDuplication(false)
	,EditedCurveSegmentIndex(-1)
	,CachedRotation(FQuat::Identity)
	,CachedScale3D(FVector::OneVector)
	,bMovingPoints(false)
	,bInsertingOnCurveControlPoints(false)
	,bRecordingMovingPoints(false)
{
	FHoudiniSplineComponentVisualizerCommands::Register();
	VisualizerActions = MakeShareable(new FUICommandList);
}

void FHoudiniSplineComponentVisualizer::OnRegister()
{
	HOUDINI_LOG_MESSAGE(TEXT("Houdini Spline Component Visualizer Registered!"));
	const auto & Commands = FHoudiniSplineComponentVisualizerCommands::Get();

	VisualizerActions->MapAction(
		Commands.CommandAddControlPoint,
		FExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::OnAddControlPoint),
		FCanExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::IsAddControlPointValid));

	VisualizerActions->MapAction(
		Commands.CommandDuplicateControlPoint,
		FExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::OnDuplicateControlPoint),
		FCanExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::IsDuplicateControlPointValid));

	VisualizerActions->MapAction(
		Commands.CommandDeleteControlPoint,
		FExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::OnDeleteControlPoint),
		FCanExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::IsDeleteControlPointValid));

	VisualizerActions->MapAction(Commands.CommandDeselectAllControlPoints,
		FExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::OnDeselectAllControlPoints),
		FCanExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::IsDeselectAllControlPointsValid));

	VisualizerActions->MapAction(Commands.CommandInsertControlPoint,
		FExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::OnInsertControlPoint),
		FCanExecuteAction::CreateSP(this, &FHoudiniSplineComponentVisualizer::IsInsertControlPointValid));
}


void FHoudiniSplineComponentVisualizer::DrawVisualization(
	const UActorComponent * Component, const FSceneView * View,
	FPrimitiveDrawInterface * PDI)
{
	const UHoudiniSplineComponent * HoudiniSplineComponent = Cast< const UHoudiniSplineComponent >(Component);
	
	if (!HoudiniSplineComponent
		|| !PDI
		|| HoudiniSplineComponent->IsPendingKill()
		|| !HoudiniSplineComponent->IsVisible()
		|| !HoudiniSplineComponent->IsHoudiniSplineVisible())
		return;

	// Note: Undo a transaction clears the active visualizer in ComponnetVisMangaer, which is private to Visualizer manager.
	//       HandleProxyForComponentVis() sets the active visualizer. So the selection will be lost after undo.

	// A Way to bypass this annoying UE4 implementation: 
	// If the drawing spline is the one being edited and an undo just happened,
	// force to trigger a 'bubble' hit proxy to re-activate the visualizer.
	if (HoudiniSplineComponent == EditedHoudiniSplineComponent && EditedHoudiniSplineComponent->bPostUndo)
	{
		EditedHoudiniSplineComponent->bPostUndo = false;

		FEditorViewportClient * FoundViewportClient = FindViewportClient(EditedHoudiniSplineComponent, View);
		HComponentVisProxy * BubbleComponentHitProxy = new HComponentVisProxy(EditedHoudiniSplineComponent);

		if (FoundViewportClient && BubbleComponentHitProxy)
		{
			FViewportClick BubbleClick(View, FoundViewportClient, FKey(), EInputEvent::IE_Axis, 0, 0);
			GUnrealEd->ComponentVisManager.HandleProxyForComponentVis(FoundViewportClient, BubbleComponentHitProxy, BubbleClick);
		}
	}
	
	static const FColor ColorNormal = FColor(255.f, 255.f, 255.f);
	static const FColor ColorNormalHandleFirst(172.f, 255.f, 172.f);
	static const FColor ColorNormalHandleSecond(254.f, 216.f, 177.f);

	static const FColor ColorSelectedHandle(255.f, 0.f, 0.f);
	static const FColor ColorSelectedHandleFirst(0.f, 192.f, 0.f);
	static const FColor ColorSelectedHandleSecond(255.f, 159.f, 0.f);

	static const float SizeGrabHandleSelected = 15.f;
	static const float SizeGrabHandleNormalLarge = 18.f;
	static const float SizeGrabHandleNormalSmall = 12.f;
	
	FVector PreviousPosition;

	if (HoudiniSplineComponent) 
	{
		const FTransform & HoudiniSplineComponentTransform = HoudiniSplineComponent->GetComponentTransform();

		const TArray< FVector > & DisplayPoints = HoudiniSplineComponent->DisplayPoints;  // not used yet
		const TArray< FTransform > & CurvePoints = HoudiniSplineComponent->CurvePoints;

		// Draw display points (simply linearly connect the control points for temporary)
		for (int32 Index = 0; Index < DisplayPoints.Num(); ++Index) 
		{
			const FVector & CurrentPoint = DisplayPoints[Index];
			FVector CurrentPosition = CurrentPoint + HoudiniSplineComponentTransform.GetLocation();
			//CurrentPosition = CurrentPoint;
			if (Index > 0) 
			{
				// Add a hitproxy for the line segment
				PDI->SetHitProxy(new HHoudiniSplineCurveSegmentVisProxy(HoudiniSplineComponent, Index));
			    // Draw a line connecting the previous point and the current point
				PDI->DrawLine(PreviousPosition, CurrentPosition, ColorNormal, SDPG_Foreground);
				PDI->SetHitProxy(nullptr);
			}

			PreviousPosition = CurrentPosition;
		}

		// Draw control points (do not draw control points if the curve is an output)
		if (!HoudiniSplineComponent->bIsOutputCurve)
		{
			for (int32 Index = 0; Index < CurvePoints.Num(); ++Index)
			{
				const FVector & ControlPoint = HoudiniSplineComponentTransform.TransformPosition(CurvePoints[Index].GetLocation());

				HHoudiniSplineControlPointVisProxy * HitProxy = new HHoudiniSplineControlPointVisProxy(HoudiniSplineComponent, Index);
				PDI->SetHitProxy(HitProxy);

				FColor DrawColor = ColorNormal;
				float DrawSize = SizeGrabHandleNormalSmall;

				if (Index == 0)
				{
					DrawColor = ColorNormalHandleFirst;
					DrawSize = SizeGrabHandleNormalLarge;
				}

				if (Index == 1)
					DrawColor = ColorNormalHandleSecond;
				

				// If this is an point that being editted
				if (EditedHoudiniSplineComponent == HoudiniSplineComponent && EditedHoudiniSplineComponent->EditedControlPointsIndexes.Contains(Index))
				{
					if (Index == 0)
					{
						DrawColor = ColorSelectedHandleFirst;
					}

					else if (Index == 1)
					{
						DrawColor = ColorSelectedHandleSecond;
						DrawSize = SizeGrabHandleSelected;
					}

					else
					{
						DrawColor = ColorSelectedHandle;
						DrawSize = SizeGrabHandleSelected;

					}
				}

				PDI->DrawPoint(ControlPoint, DrawColor, DrawSize, SDPG_Foreground);
				PDI->SetHitProxy(nullptr);
			}
		
		}
	}
}


bool FHoudiniSplineComponentVisualizer::VisProxyHandleClick(
	FEditorViewportClient* InViewportClient, HComponentVisProxy* VisProxy,
	const FViewportClick& Click)
{
	if (!InViewportClient || !VisProxy || !VisProxy->Component.IsValid())
		return false;

	const UHoudiniSplineComponent * HoudiniSplineComponent = CastChecked< const UHoudiniSplineComponent >(VisProxy->Component.Get());

	if (!HoudiniSplineComponent)
		return false;

	// Note: This is for re-activating the component visualizer an undo.
	// Return true if the hit proxy is a bubble (Neither HHoudiniSplineControlPointVisProxy nor HHoudiniSplineCurveSegmentVisProxy )
	// 
	if (!VisProxy->IsA(HHoudiniSplineControlPointVisProxy::StaticGetType()) && !VisProxy->IsA(HHoudiniSplineCurveSegmentVisProxy::StaticGetType()))
		return true;

	if (EditedHoudiniSplineComponent != HoudiniSplineComponent) 
	{
		EditedHoudiniSplineComponent = const_cast<UHoudiniSplineComponent *>(HoudiniSplineComponent);
		UHoudiniInputHoudiniSplineComponent* OuterObj = Cast<UHoudiniInputHoudiniSplineComponent>(EditedHoudiniSplineComponent->GetOuter());
		if (OuterObj && !OuterObj->IsPendingKill())
			FHoudiniEngineUtils::UpdateEditorProperties(OuterObj, true);
	}

	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return false;


	TArray<int32> & EditedControlPointsIndexes = EditedHoudiniSplineComponent->EditedControlPointsIndexes;

	bool editingCurve = false;

	// If VisProxy is a HHoudiniSplineControlPointVisProxy
	if (VisProxy->IsA(HHoudiniSplineControlPointVisProxy::StaticGetType())) 
	{
		HHoudiniSplineControlPointVisProxy * ControlPointProxy = (HHoudiniSplineControlPointVisProxy*)VisProxy;

		if (!ControlPointProxy)
			return editingCurve;

		editingCurve = true;

		// Clear the edited curve segment if a control point is clicked.
		EditedCurveSegmentIndex = -1;

		if (Click.GetKey() != EKeys::LeftMouseButton)
			return editingCurve;


		if (InViewportClient->IsCtrlPressed())
		{
			if (EditedControlPointsIndexes.Contains(ControlPointProxy->ControlPointIndex))
			{
				EditedControlPointsIndexes.Remove(ControlPointProxy->ControlPointIndex);
			}
			else
			{
				EditedControlPointsIndexes.Add(ControlPointProxy->ControlPointIndex);
			}
		}
		else
		{
			EditedControlPointsIndexes.Empty();
			EditedControlPointsIndexes.Add(ControlPointProxy->ControlPointIndex);
		}
	}
	// VisProxy is a HHoudiniSplineCurveSegmentProxy
	else if (VisProxy->IsA(HHoudiniSplineCurveSegmentVisProxy::StaticGetType())) 
	{
		//HHoudiniSplineCurveSegmentVisProxy * CurveSegmentProxy = Cast<HHoudiniSplineCurveSegmentVisProxy>(VisProxy);

		HHoudiniSplineCurveSegmentVisProxy * CurveSegmentProxy = (HHoudiniSplineCurveSegmentVisProxy*)(VisProxy);

		if (!CurveSegmentProxy)
			return false;

		editingCurve = true;

		if (Click.GetKey() == EKeys::LeftMouseButton && InViewportClient->IsAltPressed() && EditedHoudiniSplineComponent) 
		{
			// Continuesly (Alt) inserting on-curve control points is only valid with Breakpoints mode, otherwise it has to be on linear curve type.
			if (EditedHoudiniSplineComponent->CurveType != EHoudiniCurveType::Linear && EditedHoudiniSplineComponent->CurveMethod != EHoudiniCurveMethod::Breakpoints)
				return editingCurve;

			bInsertingOnCurveControlPoints = true;

			editingCurve = true;
			EditedControlPointsIndexes.Empty();

			EditedCurveSegmentIndex = CurveSegmentProxy->DisplayPointIndex;
			int32 InsertedIndex = OnInsertControlPointWithoutUpdate();

			if (InsertedIndex < 0) return false;
			EditedControlPointsIndexes.Add(InsertedIndex);

			EditedCurveSegmentIndex = -1;
			bInsertingOnCurveControlPoints = true;

			RefreshViewport();
		}
		// Insert one on-curve control point.
		else 
		{
			EditedCurveSegmentIndex = CurveSegmentProxy->DisplayPointIndex;
			return editingCurve;
		}
	}

	return editingCurve;
}

bool FHoudiniSplineComponentVisualizer::HandleInputKey(FEditorViewportClient * ViewportClient, FViewport * Viewport, FKey Key, EInputEvent Event) 
{

	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return false;

	if (Key == EKeys::Enter) 
	{
		EditedHoudiniSplineComponent->MarkInputObjectChanged();

		return true;
	}

	if (EditedHoudiniSplineComponent->NeedsToTrigerUpdate())
		return true;

	bool bHandled = false;

	if (Key == EKeys::LeftMouseButton) 
	{
		if (Event == IE_Pressed) 
		{
			bMovingPoints = true;		// Started moving points when the left mouse button is pressed
			bAllowDuplication = true;
			
		}

		if (Event == IE_Released)
		{
			bMovingPoints = false;		// Stopped moving points when the left mouse button is released
			bAllowDuplication = false;

			bRecordingMovingPoints = false;  // allow recording pt moving again

			if (IsCookOnCurveChanged(EditedHoudiniSplineComponent))
				EditedHoudiniSplineComponent->MarkInputObjectChanged();
			
		}
	}


	if (Key == EKeys::Delete) 
	{
		if (Event == IE_Pressed) return true;

		if (IsDeleteControlPointValid()) 
		{
			OnDeleteControlPoint();

			if (IsCookOnCurveChanged(EditedHoudiniSplineComponent))
				EditedHoudiniSplineComponent->MarkInputObjectChanged();
			return true;
		}
	}


	if (Event == IE_Pressed && VisualizerActions) 
	{
		if (FSlateApplication::IsInitialized())
			bHandled = VisualizerActions->ProcessCommandBindings(Key, FSlateApplication::Get().GetModifierKeys(), false);
	}

	RefreshViewport();

	return bHandled;
}

void FHoudiniSplineComponentVisualizer::EndEditing() 
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return;

	// Clear edited spline if the EndEditing() function is not called from postUndo
	if (!EditedHoudiniSplineComponent->bPostUndo)
	{
		EditedHoudiniSplineComponent->EditedControlPointsIndexes.Empty();

		EditedHoudiniSplineComponent = nullptr;
		EditedCurveSegmentIndex = -1;
	}

	//RefreshViewport();
}

bool FHoudiniSplineComponentVisualizer::GetWidgetLocation(const FEditorViewportClient* ViewportClient, FVector& OutLocation) const
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return false;
	
	TArray<int32> & EditedControlPointsIndexes = EditedHoudiniSplineComponent->EditedControlPointsIndexes;

	if (EditedControlPointsIndexes.Num() <= 0)
		return false;

	const TArray< FTransform > & CurvePoints = EditedHoudiniSplineComponent->CurvePoints;

	// Set the widget location to the center of mass of the selected control points
	FVector CenterLocation = FVector::ZeroVector;
	
	for (int i = 0; i < EditedControlPointsIndexes.Num(); ++i) 
	{
		CenterLocation += CurvePoints[EditedControlPointsIndexes[i]].GetLocation();
	}

	CenterLocation /= EditedControlPointsIndexes.Num();
	OutLocation = EditedHoudiniSplineComponent->GetComponentTransform().TransformPosition(CenterLocation);

	return true;
}

bool FHoudiniSplineComponentVisualizer::HandleInputDelta(
	FEditorViewportClient* ViewportClient, FViewport* Viewport,
	FVector& DeltaTranslate, FRotator& DeltaRotate,
	FVector& DeltaScale) 
{
	if (!ViewportClient || !EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return false;

	if (EditedHoudiniSplineComponent->NeedsToTrigerUpdate())
		return true;

	TArray<int32> & EditedControlPointsIndexes = EditedHoudiniSplineComponent->EditedControlPointsIndexes;

	if (EditedControlPointsIndexes.Num() <= 0)
		return false;

	if (ViewportClient->IsAltPressed() && bAllowDuplication) 
	{
		OnDuplicateControlPoint();
		bAllowDuplication = false;
	}
	else
	{
		if (!bRecordingMovingPoints)
		{
			FScopedTransaction Transaction(
				TEXT(HOUDINI_MODULE_RUNTIME),
				LOCTEXT("HoudiniSplineComponentMovingPointsTransaction", "Houdini Spline Component: Moving curve points."),
				EditedHoudiniSplineComponent->GetOuter(), true);

			EditedHoudiniSplineComponent->Modify();

			bRecordingMovingPoints = true;
		}
	}


	TArray <FTransform> & CurvePoints = EditedHoudiniSplineComponent->CurvePoints;

	const FTransform & HoudiniSplineComponentTransform = EditedHoudiniSplineComponent->GetComponentTransform();

	for (int i = 0; i < EditedControlPointsIndexes.Num(); ++i ) 
	{

		FTransform  CurrentPoint = EditedHoudiniSplineComponent->CurvePoints[EditedControlPointsIndexes[i]];
	
		if (!DeltaTranslate.IsZero()) 
		{
			FVector OldWorldPosition = HoudiniSplineComponentTransform.TransformPosition(CurrentPoint.GetLocation());
			FVector NewWorldPosition = OldWorldPosition + DeltaTranslate;
			FVector NewLocalPosition = HoudiniSplineComponentTransform.InverseTransformPosition(NewWorldPosition);
			CurrentPoint.SetLocation( NewLocalPosition );
		}

		if (!DeltaRotate.IsZero())
		{
			FQuat OldWorldRotation = HoudiniSplineComponentTransform.GetRotation() * CurrentPoint.GetRotation();
			FQuat NewWorldRotation = DeltaRotate.Quaternion() * OldWorldRotation;
			FQuat NewLocalRotation = HoudiniSplineComponentTransform.GetRotation().Inverse() * NewWorldRotation;
			CurrentPoint.SetRotation(NewLocalRotation);
		}

		if (!DeltaScale.IsZero()) 
		{
			FVector NewScale = CurrentPoint.GetScale3D() * (FVector(1.f, 1.f, 1.f) + DeltaScale);
			CurrentPoint.SetScale3D(NewScale);
		}


		EditedHoudiniSplineComponent->EditPointAtindex(CurrentPoint, EditedControlPointsIndexes[i]);
	}

	RefreshViewport();

	return true;
}

TSharedPtr<SWidget> FHoudiniSplineComponentVisualizer::GenerateContextMenu() const
{
	FHoudiniEngineEditor& HoudiniEngineEditor = FHoudiniEngineEditor::Get();
	//FName StyleSetName = FHoudiniEngineStyle::GetStyleSetName();
	
	FMenuBuilder MenuBuilder(true, VisualizerActions);
	MenuBuilder.BeginSection("houdini spline menu section");
	// Create the context menu section
	{
		if (EditedHoudiniSplineComponent && !EditedHoudiniSplineComponent->IsPendingKill()) 
		{
		
			MenuBuilder.AddMenuEntry(
				FHoudiniSplineComponentVisualizerCommands::Get().CommandAddControlPoint,
				NAME_None, TAttribute<FText>(), TAttribute<FText>(),
				FSlateIcon("Hello Style", "HoudiniEngine.HoudiniEngineLogo"));

			MenuBuilder.AddMenuEntry(
				FHoudiniSplineComponentVisualizerCommands::Get().CommandDuplicateControlPoint,
				NAME_None, TAttribute< FText >(), TAttribute< FText >(),
				FSlateIcon("Hello Style", "HoudiniEngine.HoudiniEngineLogo"));

			MenuBuilder.AddMenuEntry(
				FHoudiniSplineComponentVisualizerCommands::Get().CommandDeleteControlPoint,
				NAME_None, TAttribute< FText >(), TAttribute< FText >(),
				FSlateIcon("Hello Style", "HoudiniEngine.HoudiniEngineLogo"));

			MenuBuilder.AddMenuEntry(
				FHoudiniSplineComponentVisualizerCommands::Get().CommandDeselectAllControlPoints,
				NAME_None, TAttribute< FText >(), TAttribute< FText >(),
				FSlateIcon("Hello Style", "HoudiniEngine.HoudiniEngineLogo"));

			MenuBuilder.AddMenuEntry(
				FHoudiniSplineComponentVisualizerCommands::Get().CommandInsertControlPoint,
				NAME_None, TAttribute< FText >(), TAttribute< FText >(),
				FSlateIcon("Hello Style", "HoudiniEngine.HoudiniEngineLogo"));

		}
	}

	MenuBuilder.EndSection();
	TSharedPtr<SWidget> MenuWidget = MenuBuilder.MakeWidget();
	return MenuWidget;
}

// Used by alt-pressed on-curve control port insertion.
// We don't want it to be cooked before finishing editing.
// * Need to call WaitForHoudiniInputUpdate() after done.
int32 FHoudiniSplineComponentVisualizer::OnInsertControlPointWithoutUpdate() 
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill()) 
		return -1;

	TArray<FTransform> & CurvePoints = EditedHoudiniSplineComponent->CurvePoints;
	TArray<FVector> & DisplayPoints = EditedHoudiniSplineComponent->DisplayPoints;

	if (EditedCurveSegmentIndex >= DisplayPoints.Num())
		return -1;

	// ... //
	int InsertAfterIndex = 0;

	TArray<int32> & DisplayPointIndexDivider = EditedHoudiniSplineComponent->DisplayPointIndexDivider;
	for (int itr = 0; itr < DisplayPointIndexDivider.Num(); ++itr)
	{
		if (DisplayPointIndexDivider[itr] >= EditedCurveSegmentIndex)
		{
			InsertAfterIndex = itr;
			break;
		}
	}
	// ... //
	
	if (InsertAfterIndex >= CurvePoints.Num()) return -1;

	FTransform NewPoint = CurvePoints[InsertAfterIndex];
	NewPoint.SetLocation(DisplayPoints[EditedCurveSegmentIndex]);
	// To Do: Should interpolate the rotation and scale as well here.
	// ...

	// Insert new control point on curve, and add it to selected CP.
	int32 NewPointIndex = AddControlPointAfter(NewPoint, InsertAfterIndex);

	// Don't have to reconstruct the index divider each time.
	//EditedHoudiniSplineComponent->Construct(EditedHoudiniSplineComponent->DisplayPoints);
	EditedHoudiniSplineComponent->DisplayPointIndexDivider.Insert(EditedCurveSegmentIndex, InsertAfterIndex);

	if (IsCookOnCurveChanged(EditedHoudiniSplineComponent))
		EditedHoudiniSplineComponent->MarkInputObjectChanged();

	return NewPointIndex;
}

void FHoudiniSplineComponentVisualizer::OnInsertControlPoint() 
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return;

	int32 NewPointIndex = OnInsertControlPointWithoutUpdate();

	if (NewPointIndex < 0) return;


	EditedHoudiniSplineComponent->EditedControlPointsIndexes.Add(NewPointIndex);

	RefreshViewport();

	if (IsCookOnCurveChanged(EditedHoudiniSplineComponent))
		EditedHoudiniSplineComponent->MarkInputObjectChanged();
}

bool FHoudiniSplineComponentVisualizer::IsInsertControlPointValid() const
{
	return EditedCurveSegmentIndex >= 0;
}

void FHoudiniSplineComponentVisualizer::OnAddControlPoint()
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return;
	
	TArray<int32> & EditedControlPointsIndexes = EditedHoudiniSplineComponent->EditedControlPointsIndexes;

	// Transaction for Undo/Redo
	FScopedTransaction Transaction(
		TEXT(HOUDINI_MODULE_RUNTIME),
		LOCTEXT("HoudiniSplineComponentInsertingPointsTransaction", "Houdini Spline Component: Inserting curve points."),
		EditedHoudiniSplineComponent->GetOuter(), true);
	
	EditedHoudiniSplineComponent->Modify();

	EditedControlPointsIndexes.Sort();

	const TArray<FTransform>  & CurvePoints = EditedHoudiniSplineComponent->CurvePoints;

	TArray<int32> tNewSelectedPoints;

	if (EditedControlPointsIndexes.Num() == 1) 
	{
		FTransform Point = CurvePoints[EditedControlPointsIndexes[0]];
		FTransform NewTransform = FTransform::Identity;
		FVector Location = Point.GetLocation();
		//FQuat Rotation = Point.GetRotation();
		//FVector Scale = Point.GetScale3D();

		NewTransform.SetLocation(Location + 1.f);
		//NewTransform.SetRotation(Rotation);
		//NewTransform.SetScale3D(Scale);

		

		int32 NewPointIndex = AddControlPointAfter(NewTransform, EditedControlPointsIndexes[0]);
		tNewSelectedPoints.Add(NewPointIndex);
	}
	else
	{
		int IndexIncrement = 0;
		int CurrentPointIndex, LastPointIndex;
		FTransform CurrentPoint, LastPoint;

		for (int32 n = 0; n < EditedControlPointsIndexes.Num(); ++n)
		{
			// Insert a new point between each adjacent pair of points
			if (n > 0)
			{
				CurrentPointIndex = EditedControlPointsIndexes[n];
				LastPointIndex = EditedControlPointsIndexes[n - 1];
				CurrentPoint = CurvePoints[CurrentPointIndex + IndexIncrement];
				LastPoint = CurvePoints[LastPointIndex + IndexIncrement];

				// Insert a point in the middle of LastPoint and CurrentPoint
				FVector NewPointLocation = LastPoint.GetLocation() + (CurrentPoint.GetLocation() - LastPoint.GetLocation()) / 2.f;
				FVector NewPointScale = LastPoint.GetScale3D() + (CurrentPoint.GetScale3D() - LastPoint.GetScale3D()) / 2.f;
				FQuat NewPointRotation = FQuat::Slerp(LastPoint.GetRotation(), CurrentPoint.GetRotation(), .5f);

				FTransform NewTransform = FTransform::Identity;
				NewTransform.SetLocation(NewPointLocation);
				NewTransform.SetScale3D(NewPointScale);
				NewTransform.SetRotation(NewPointRotation);

				int32 NewPointIndex = AddControlPointAfter(NewTransform, EditedControlPointsIndexes[n - 1] + IndexIncrement);
				tNewSelectedPoints.Add(NewPointIndex);
				

				IndexIncrement += 1;
			}
		}
	}

	EditedControlPointsIndexes.Empty();
	EditedControlPointsIndexes = tNewSelectedPoints;

	if (IsCookOnCurveChanged(EditedHoudiniSplineComponent))
		EditedHoudiniSplineComponent->MarkInputObjectChanged();

	RefreshViewport();
}


bool FHoudiniSplineComponentVisualizer::IsAddControlPointValid() const
{
	return EditedHoudiniSplineComponent && !EditedHoudiniSplineComponent->IsPendingKill() && 
		EditedHoudiniSplineComponent->EditedControlPointsIndexes.Num() > 0;
}

void FHoudiniSplineComponentVisualizer::OnDeleteControlPoint()
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return;

	TArray<int32> & EditedControlPointsIndexes = EditedHoudiniSplineComponent->EditedControlPointsIndexes;

	if (EditedControlPointsIndexes.Num() <= 0)
		return;

	// Transaction for Undo/Redo
	FScopedTransaction Transaction(
		TEXT(HOUDINI_MODULE_RUNTIME),
		LOCTEXT("HoudiniSplineComponentDeletingPointsTransaction", "Houdini Spline Component: Deleting curve points."),
		EditedHoudiniSplineComponent->GetOuter(), true);
	EditedHoudiniSplineComponent->Modify();

	EditedControlPointsIndexes.Sort();

	int32 SelectedIndexAfterDelete = EditedControlPointsIndexes[0] - 1;
	SelectedIndexAfterDelete = FMath::Max(SelectedIndexAfterDelete, 0);

	for (int32 n = EditedControlPointsIndexes.Num() - 1; n >= 0; --n) 
	{
		int32 RemoveIndex = EditedControlPointsIndexes[n];
		EditedHoudiniSplineComponent->RemovePointAtIndex(RemoveIndex);
		
	}

	EditedControlPointsIndexes.Empty();
	OnDeselectAllControlPoints();
	EditedControlPointsIndexes.Add(SelectedIndexAfterDelete);

	if (IsCookOnCurveChanged(EditedHoudiniSplineComponent))
		EditedHoudiniSplineComponent->MarkInputObjectChanged();

	// Force refresh the viewport after deleting points to ensure the consistency of HitProxy
	RefreshViewport();
	
}

bool FHoudiniSplineComponentVisualizer::IsDeleteControlPointValid() const
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return false;

	TArray<int32> & EditedControlPointsIndexes = EditedHoudiniSplineComponent->EditedControlPointsIndexes;

	if (EditedControlPointsIndexes.Num() <= 0)
		return false;
	
	// We only allow the number of Control Points is at least 2 after delete
	if (EditedHoudiniSplineComponent->GetCurvePointCount() - EditedControlPointsIndexes.Num() < 2)
		return false;

	return true;
}

void FHoudiniSplineComponentVisualizer::OnDuplicateControlPoint()
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return;

	TArray<int32> & EditedControlPointsIndexes = EditedHoudiniSplineComponent->EditedControlPointsIndexes;

	if (EditedControlPointsIndexes.Num() <= 0)
		return;

	// Transaction for Undo/Redo
	FScopedTransaction Transaction(
		TEXT(HOUDINI_MODULE_RUNTIME),
		LOCTEXT("HoudiniSplineComponentDuplicatingPointsTransaction", "Houdini Spline Component: Duplicating curve points."),
		EditedHoudiniSplineComponent->GetOuter(), true);
	EditedHoudiniSplineComponent->Modify();
	
	const TArray<FTransform> & CurvePoints = EditedHoudiniSplineComponent->CurvePoints;

	EditedControlPointsIndexes.Sort();

	TArray<int32> tNewSelectedPoints;
	int IncrementIndex = 0;
	for (int n = 0; n < EditedControlPointsIndexes.Num(); ++n) 
	{
		int32 IndexAfter = EditedControlPointsIndexes[n] + IncrementIndex;
		FTransform CurrentPoint = CurvePoints[IndexAfter];
		if (IndexAfter == 0)
			IndexAfter = -1;
		int32 NewPointIndex = AddControlPointAfter(CurrentPoint, IndexAfter);
		tNewSelectedPoints.Add(NewPointIndex);
		IncrementIndex ++;
	}

	EditedControlPointsIndexes.Empty();
	EditedControlPointsIndexes = tNewSelectedPoints;

	EditedHoudiniSplineComponent->MarkModified(true);

	RefreshViewport();
}

bool FHoudiniSplineComponentVisualizer::IsDuplicateControlPointValid() const
{
	if(!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill() 
		|| EditedHoudiniSplineComponent->EditedControlPointsIndexes.Num() == 0)
		return false;

	return true;
}

void FHoudiniSplineComponentVisualizer::OnDeselectAllControlPoints() 
{
	if (EditedHoudiniSplineComponent && !EditedHoudiniSplineComponent->IsPendingKill())
		EditedHoudiniSplineComponent->EditedControlPointsIndexes.Empty();
}

bool FHoudiniSplineComponentVisualizer::IsDeselectAllControlPointsValid() const
{
	if (EditedHoudiniSplineComponent && !EditedHoudiniSplineComponent->IsPendingKill())
		return EditedHoudiniSplineComponent->EditedControlPointsIndexes.Num() > 0;

	return false;
}

int32 FHoudiniSplineComponentVisualizer::AddControlPointAfter(const FTransform & NewPoint, const int32 & nIndex)
{
	if (!EditedHoudiniSplineComponent || EditedHoudiniSplineComponent->IsPendingKill())
		return nIndex;

	const TArray<FTransform> & CurvePoints = EditedHoudiniSplineComponent->CurvePoints;
	
	int32 NewControlPointIndex = nIndex + 1;

	if (NewControlPointIndex == CurvePoints.Num())
		EditedHoudiniSplineComponent->AppendPoint(NewPoint);
	else
		EditedHoudiniSplineComponent->InsertPointAtIndex(NewPoint, NewControlPointIndex);

	// Return the index of the inserted control point
	return NewControlPointIndex;
}

void FHoudiniSplineComponentVisualizer::RefreshViewport()
{
	if (GEditor)
		GEditor->RedrawLevelEditingViewports(true);
}

// Find the EditorViewportClient of the viewport where the Houdini Spline Component lives in
FEditorViewportClient *
FHoudiniSplineComponentVisualizer::FindViewportClient(const UHoudiniSplineComponent * InHoudiniSplineComponent, const FSceneView * View)
{
	if (!View || !InHoudiniSplineComponent)
		return nullptr;

	UWorld * World = InHoudiniSplineComponent->GetWorld();
	uint32 ViewKey = View->GetViewKey();

	const TArray<FEditorViewportClient*> & AllViewportClients = GUnrealEd->GetAllViewportClients();

	for (auto & NextViewportClient : AllViewportClients) 
	{
		if (!NextViewportClient)
			continue;

		if (NextViewportClient->GetWorld() != World)
			continue;

		// Found the viewport client which matches the unique key of the current scene view
		if (NextViewportClient->ViewState.GetReference()->GetViewKey() == ViewKey)
			return NextViewportClient;
	}

	return nullptr;
}

bool
FHoudiniSplineComponentVisualizer::IsCookOnCurveChanged(UHoudiniSplineComponent * InHoudiniSplineComponent) 
{

	if (!InHoudiniSplineComponent)
		return true;

	UHoudiniInputObject * InputObject = Cast<UHoudiniInputObject>(EditedHoudiniSplineComponent->GetOuter());
	if (!InputObject)
		return true;

	UHoudiniInput * Input = Cast<UHoudiniInput>(InputObject->GetOuter());

	if (!Input)
		return true;

	return Input->GetCookOnCurveChange();
};

#undef LOCTEXT_NAMESPACE