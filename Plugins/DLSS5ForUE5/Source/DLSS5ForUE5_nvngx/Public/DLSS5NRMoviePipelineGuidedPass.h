#pragma once
#include "CoreMinimal.h"
#include "MoviePipelineDeferredPasses.h"
#include "DLSS5NRMoviePipelineGuidedPass.generated.h"

// Inherit Epic's complete render pass rather than replacing its accumulation code.
UCLASS(BlueprintType)
class DLSS5FORUE5_NVNGX_API UDLSS5NRMoviePipelineDeferredPass : public UMoviePipelineDeferredPassBase
{
    GENERATED_BODY()
  protected:
    virtual void AddViewExtensions(FSceneViewFamilyContext &Context,
                                   FMoviePipelineRenderPassMetrics &Metrics) override;
#if WITH_EDITOR
    virtual FText GetDisplayText() const override
    {
        return NSLOCTEXT("DLSS5", "GuidedDeferred", "DLSS5 NR - Deferred");
    }
#endif
};

UCLASS(BlueprintType)
class DLSS5FORUE5_NVNGX_API UDLSS5NRMoviePipelinePathTracerPass : public UMoviePipelineDeferredPass_PathTracer
{
    GENERATED_BODY()
  protected:
    virtual void AddViewExtensions(FSceneViewFamilyContext &Context,
                                   FMoviePipelineRenderPassMetrics &Metrics) override;
#if WITH_EDITOR
    virtual FText GetDisplayText() const override
    {
        return NSLOCTEXT("DLSS5", "GuidedPathTracer", "DLSS5 NR - Path Tracing");
    }
#endif
};
