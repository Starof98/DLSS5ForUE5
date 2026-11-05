#pragma once
#include "CoreMinimal.h"
#include "RenderTargetPool.h"
#include "MovieRenderPipelineDataTypes.h"
#include "RHIGPUReadback.h"

namespace DLSS5NRMRQ
{
struct FGuides
{
    TRefCountPtr<IPooledRenderTarget> Depth, Motion;
    FIntPoint Size = FIntPoint::ZeroValue;
    int32 Output = MIN_int32;
    int32 Shot = -1;
    int32 Camera = -1;
    bool bEngineVelocity = false;
    bool bReset = true;
    float PreExposure = 1;
    FVector2f Jitter = FVector2f::ZeroVector;
    float FOV = 0;
    FString Error;
    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> Statistics;
};
void SetGuideMode(int32 Mode); // Game thread, flush before changing.
void ClearGuides();
TSharedPtr<FGuides, ESPMode::ThreadSafe> TakeGuides(const FMoviePipelineRenderPassMetrics &Metrics,
                                                    const FString &Pass);
void AttachGuides(FSceneViewFamilyContext &Context, const FMoviePipelineRenderPassMetrics &Metrics,
                  const FString &Pass);
} // namespace DLSS5NRMRQ
