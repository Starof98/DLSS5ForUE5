#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DLSS5NRPresetLibrary.generated.h"

class UMoviePipelinePrimaryConfig;

/** Editor helpers for creating new presets; existing configurations are never rewritten. */
UCLASS()
class DLSS5FORUE5EDITOR_API UDLSS5NRPresetLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
  public:
    /** Creates a uniquely named asset under /Game/DLSS5/Presets. Save it after adjusting the output. */
    UFUNCTION(BlueprintCallable, Category = "DLSS5|Movie Render Queue")
    static UMoviePipelinePrimaryConfig *CreateMRQPreset(bool bPathTracing, bool bOpenInContentBrowser = true);
};
