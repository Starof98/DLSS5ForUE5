#include "DLSS5NRMoviePipelineGuides.h"
#include "DLSS5NRMoviePipelineGuidedPass.h"
#include "DLSS5NRMoviePipelineBridge.h"
#include "DLSS5ForUE5Module.h"
#include "SceneViewExtension.h"
#include "SceneRendering.h"
#include "SceneTexturesConfig.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"
#include "Misc/ScopeLock.h"
#include "Misc/AutomationTest.h"
#include "RHIGPUReadback.h"

class FDLSS5NRMRQGuidesCS : public FGlobalShader
{
  public:
    DECLARE_GLOBAL_SHADER(FDLSS5NRMRQGuidesCS);
    SHADER_USE_PARAMETER_STRUCT(FDLSS5NRMRQGuidesCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
    SHADER_PARAMETER(FMatrix44f, EngineClipToPrevious)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SourceDepth)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SourceVelocity)
    SHADER_PARAMETER(FIntPoint, SourceMin)
    SHADER_PARAMETER(FIntPoint, SourceSize)
    SHADER_PARAMETER(FIntPoint, OutputSize)
    SHADER_PARAMETER(FVector2f, ProjectionJitter)
    SHADER_PARAMETER(FMatrix44f, OutputClipToPrevious)
    SHADER_PARAMETER(uint32, UseEngineVelocity)
    SHADER_PARAMETER(uint32, ResetMotion)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, GuideDepth)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, GuideMotion)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters &P)
    {
        return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
    }
};
IMPLEMENT_GLOBAL_SHADER(FDLSS5NRMRQGuidesCS, "/DLSS5NR/Private/DLSS5NRMRQGuides.usf", "MRQGuidesCS",
                        SF_Compute);

class FDLSS5NRMRQStatisticsCS : public FGlobalShader
{
  public:
    DECLARE_GLOBAL_SHADER(FDLSS5NRMRQStatisticsCS);
    SHADER_USE_PARAMETER_STRUCT(FDLSS5NRMRQStatisticsCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, StatsDepth)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, StatsMotion)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SourceVelocity)
    SHADER_PARAMETER(FIntPoint, OutputSize)
    SHADER_PARAMETER(FIntPoint, SourceMin)
    SHADER_PARAMETER(FIntPoint, SourceSize)
    SHADER_PARAMETER(FVector2f, ProjectionJitter)
    SHADER_PARAMETER(uint32, UseEngineVelocity)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, GuideStatistics)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters &P)
    {
        return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
    }
};
IMPLEMENT_GLOBAL_SHADER(FDLSS5NRMRQStatisticsCS, "/DLSS5NR/Private/DLSS5NRMRQGuides.usf",
                        "MRQGuideStatisticsCS", SF_Compute);

namespace DLSS5NRMRQ
{
namespace
{
int32 GuideMode = 0;
FCriticalSection GuidesMutex;
TMap<FString, TSharedPtr<FGuides, ESPMode::ThreadSafe>> Pending;
struct FCameraHistory
{
    FMatrix WorldToClip = FMatrix::Identity;
    int32 Output = MIN_int32;
    FIntPoint Size = FIntPoint::ZeroValue;
};
TMap<FString, FCameraHistory> Cameras; // Render thread only.
FString StreamKey(const FMoviePipelineRenderPassMetrics &M, const FString &Pass)
{
    return FString::Printf(TEXT("%d|%d|%s"), M.OutputState.ShotIndex, M.OutputState.CameraIndex, *Pass);
}
FString FrameKey(const FMoviePipelineRenderPassMetrics &M, const FString &Pass)
{
    return StreamKey(M, Pass) + FString::Printf(TEXT("|%d"), M.OutputState.OutputFrameNumber);
}

class FGuideExtension final : public FSceneViewExtensionBase
{
  public:
    FGuideExtension(const FAutoRegister &Auto, const FMoviePipelineRenderPassMetrics &M, const FString &P,
                    int32 Mode)
        : FSceneViewExtensionBase(Auto), Metrics(M), Pass(P), ModeValue(Mode)
    {
    }
    // Added only to its own family; never leak a capture into viewport/another camera.
    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext &) const override
    {
        return false;
    }
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass P, const FSceneView &V,
                                               FPostProcessingPassDelegateArray &Callbacks,
                                               bool Enabled) override
    {
        if (P == EPostProcessingPass::Tonemap && Enabled && V.bIsOfflineRender)
            Callbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FGuideExtension::Capture));
    }

  private:
    FMoviePipelineRenderPassMetrics Metrics;
    FString Pass;
    int32 ModeValue;
    bool bCaptured = false;
    FScreenPassTexture Capture(FRDGBuilder &Graph, const FSceneView &View,
                               const FPostProcessMaterialInputs &Inputs)
    {
        const auto Color =
            FScreenPassTexture::CopyFromSlice(Graph, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
        if (bCaptured)
            return Color;
        bCaptured = true;
        auto G = MakeShared<FGuides, ESPMode::ThreadSafe>();
        {
            FScopeLock Lock(&GuidesMutex);
            if (Pending.Contains(FrameKey(Metrics, Pass)))
            {
                Pending[FrameKey(Metrics, Pass)]->Error = TEXT("Duplicate guide capture for output frame");
                return Color;
            }
            if (Pending.Num() >= 16)
            {
                UE_LOG(LogDLSS5, Error,
                       TEXT("NRGUIDES pending queue full; render must abort rather than reuse guides"));
                return Color;
            }
            // Own extraction destinations until the graph has executed, even on failure.
            Pending.Add(FrameKey(Metrics, Pass), G);
        }
        G->Output = Metrics.OutputState.OutputFrameNumber;
        G->Shot = Metrics.OutputState.ShotIndex;
        G->Camera = Metrics.OutputState.CameraIndex;
        G->Size = Color.ViewRect.Size();
        G->FOV = View.FOV;
        G->PreExposure = static_cast<const FViewInfo &>(View).PreExposure;
        const FIntRect Rect = static_cast<const FViewInfo &>(View).ViewRect;
        FRDGTextureRef Depth = nullptr, Velocity = nullptr;
        if (auto *UB = Inputs.SceneTextures.SceneTextures.GetUniformBuffer())
            if (const auto *T = UB->GetContents())
            {
                Depth = T->SceneDepthTexture;
                Velocity = T->GBufferVelocityTexture;
            }
        if (!View.OverrideOutputFrameIndexValue.IsSet() ||
            View.OverrideOutputFrameIndexValue.GetValue() != uint32(G->Output))
            G->Error = TEXT("View output-frame override does not match MRQ metrics");
        else if (Metrics.TemporalSampleCount != 1 || Metrics.GetTileCount() != 1 ||
                 Metrics.AntiAliasingMethod != AAM_None)
            G->Error = TEXT("MRQ guides require temporal=1, tiles=1 and AA=None");
        else if (View.Family->Views.Num() != 1 || !View.IsPerspectiveProjection() ||
                 View.Family->EngineShowFlags.DepthOfField || View.Family->EngineShowFlags.MotionBlur)
            G->Error = TEXT("MRQ guides require one perspective view and Disable Multisample Effects ON "
                            "(DOF/motion blur off)");
        else if (!Depth || Rect.Min.X < 0 || Rect.Min.Y < 0 || Rect.Width() <= 1 || Rect.Height() <= 1 ||
                 Depth->Desc.Extent.X < Rect.Max.X || Depth->Desc.Extent.Y < Rect.Max.Y ||
                 !Depth->HasBeenProduced())
            G->Error = TEXT("No produced full-resolution scene depth: guide capture rejected");
        else if (Rect.Size() != G->Size)
            G->Error = TEXT(
                "MRQ guides require screen percentage 100; remove DLSS/DLAA/TSR upscaling from guide test");
        const bool HasVelocity = Velocity && Velocity->Desc.Extent.X >= Rect.Max.X &&
                                 Velocity->Desc.Extent.Y >= Rect.Max.Y && Velocity->Desc.Extent.X > 1 &&
                                 Velocity->HasBeenProduced();
        if (G->Error.IsEmpty() && ModeValue == 2 && !HasVelocity)
            G->Error = TEXT("Engine motion requested but no produced velocity buffer. Use Camera Depth mode "
                            "to test static scenes; animated objects need real engine velocity");
        if (G->Error.IsEmpty())
        {
            const FMatrix Current =
                View.ViewMatrices.GetWorldToView() * View.ViewMatrices.GetViewToClipNoAA();
            FCameraHistory &Prev = Cameras.FindOrAdd(StreamKey(Metrics, Pass));
            G->bReset = Prev.Output == MIN_int32 || int64(G->Output) != int64(Prev.Output) + 1 ||
                        Prev.Size != G->Size || Metrics.bCameraCut;
            const FMatrix ToPrev = G->bReset ? FMatrix::Identity : Current.Inverse() * Prev.WorldToClip;
            G->bEngineVelocity = ModeValue == 2;
            G->Jitter = FVector2f(View.ViewMatrices.GetTemporalAAJitter());
            auto Make = [&](EPixelFormat Format, const TCHAR *Name) {
                return Graph.CreateTexture(
                    FRDGTextureDesc::Create2D(G->Size, Format, FClearValueBinding::Black,
                                              TexCreate_ShaderResource | TexCreate_UAV),
                    Name);
            };
            auto D = Make(PF_R32_FLOAT, TEXT("DLSS5NR.MRQDepth"));
            auto M = Make(PF_G32R32F, TEXT("DLSS5NR.MRQOutputFrameMotion"));
            auto *P = Graph.AllocParameters<FDLSS5NRMRQGuidesCS::FParameters>();
            P->EngineClipToPrevious =
                static_cast<const FViewInfo &>(View).CachedViewUniformShaderParameters->ClipToPrevClip;
            P->SourceDepth = Depth;
            P->SourceVelocity = HasVelocity ? Velocity : Depth;
            P->SourceMin = Rect.Min;
            P->SourceSize = Rect.Size();
            P->OutputSize = G->Size;
            P->ProjectionJitter = G->Jitter;
            P->OutputClipToPrevious = FMatrix44f(ToPrev);
            P->UseEngineVelocity = G->bEngineVelocity ? 1 : 0;
            P->ResetMotion = G->bReset ? 1 : 0;
            P->GuideDepth = Graph.CreateUAV(D);
            P->GuideMotion = Graph.CreateUAV(M);
            TShaderMapRef<FDLSS5NRMRQGuidesCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("DLSS5NR MRQ output-frame guides"), Shader, P,
                                         FComputeShaderUtils::GetGroupCount(G->Size, FIntPoint(8, 8)));
            auto StatsBuffer =
                Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 256),
                                   TEXT("DLSS5NR.MRQGuideStatistics"));
            auto *S = Graph.AllocParameters<FDLSS5NRMRQStatisticsCS::FParameters>();
            S->StatsDepth = D;
            S->StatsMotion = M;
            S->SourceVelocity = HasVelocity ? Velocity : Depth;
            S->OutputSize = G->Size;
            S->SourceMin = Rect.Min;
            S->SourceSize = Rect.Size();
            S->ProjectionJitter = G->Jitter;
            S->UseEngineVelocity = G->bEngineVelocity ? 1 : 0;
            S->GuideStatistics = Graph.CreateUAV(StatsBuffer);
            TShaderMapRef<FDLSS5NRMRQStatisticsCS> StatsShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("DLSS5NR MRQ guide sample statistics"),
                                         StatsShader, S, FIntVector(2, 2, 1));
            G->Statistics =
                MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("DLSS5NR.MRQGuideStats"));
            AddEnqueueCopyPass(Graph, G->Statistics.Get(), StatsBuffer, 256 * sizeof(FVector4f));
            Graph.QueueTextureExtraction(D, &G->Depth, ERHIAccess::SRVCompute);
            Graph.QueueTextureExtraction(M, &G->Motion, ERHIAccess::SRVCompute);
            Prev.WorldToClip = Current;
            Prev.Output = G->Output;
            Prev.Size = G->Size;
        }
        if (!G->Error.IsEmpty())
        {
            UE_LOG(LogDLSS5, Error, TEXT("NRGUIDES capture output=%d shot=%d camera=%d: %s"), G->Output,
                   G->Shot, G->Camera, *G->Error);
        }
        else
        {
            UE_LOG(
                LogDLSS5, Display,
                TEXT(
                    "NRGUIDES capture output=%d shot=%d camera=%d size=%dx%d spatial=0/%d temporal=1 "
                    "engineVelocity=%d reset=%d jitter=(%.6f,%.6f) preExposure=%.6f fov=%.6f worldPaused=%d"),
                G->Output, G->Shot, G->Camera, G->Size.X, G->Size.Y, Metrics.SpatialSampleCount,
                G->bEngineVelocity ? 1 : 0, G->bReset ? 1 : 0, G->Jitter.X, G->Jitter.Y, G->PreExposure,
                G->FOV, Metrics.bWorldIsPaused ? 1 : 0);
        }
        return Color;
    }
};
} // namespace
void SetGuideMode(int32 Mode)
{
    check(IsInGameThread());
    GuideMode = Mode;
}
void ClearGuides()
{
    check(IsInGameThread());
    ENQUEUE_RENDER_COMMAND(DLSS5NRClearGuideHistory)([](FRHICommandListImmediate &Cmd) {
        Cmd.SubmitAndBlockUntilGPUIdle();
        Cameras.Reset();
    });
    FlushRenderingCommands();
    FScopeLock Lock(&GuidesMutex);
    Pending.Reset();
}
TSharedPtr<FGuides, ESPMode::ThreadSafe> TakeGuides(const FMoviePipelineRenderPassMetrics &M,
                                                    const FString &Pass)
{
    TSharedPtr<FGuides, ESPMode::ThreadSafe> Found;
    {
        FScopeLock Lock(&GuidesMutex);
        Pending.RemoveAndCopyValue(FrameKey(M, Pass), Found);
    }
    if (Found && Found->Statistics)
    {
        ENQUEUE_RENDER_COMMAND(DLSS5NRMRQLogGuideStatistics)([Found](FRHICommandListImmediate &Cmd) {
            if (!Found->Statistics->IsReady())
                Cmd.SubmitAndBlockUntilGPUIdle();
            if (!Found->Statistics->IsReady())
            {
                Found->Error = TEXT("Guide statistics GPU readback unavailable");
                return;
            }
            const auto *Data =
                static_cast<const FVector4f *>(Found->Statistics->Lock(256 * sizeof(FVector4f)));
            if (!Data)
            {
                Found->Statistics->Unlock();
                Found->Error = TEXT("Guide statistics readback failed");
                return;
            }
            float MinD = 1, MaxD = 0, MaxMotion = 0;
            int32 Coverage = 0, ValidVelocity = 0;
            for (int32 I = 0; I < 256; ++I)
            {
                MinD = FMath::Min(MinD, Data[I].X);
                MaxD = FMath::Max(MaxD, Data[I].X);
                Coverage += Data[I].X > 0 ? 1 : 0;
                ValidVelocity += Data[I].W > 0 ? 1 : 0;
                MaxMotion = FMath::Max(MaxMotion, FMath::Sqrt(Data[I].Y * Data[I].Y + Data[I].Z * Data[I].Z));
            }
            Found->Statistics->Unlock();
            UE_LOG(LogDLSS5, Display,
                   TEXT("NRGUIDES stats output=%d shot=%d camera=%d depthRange=(%.8f,%.8f) "
                        "depthCoverage=%d/256 engineVelocityCoverage=%d/256 maxMotionPixels=%.6f"),
                   Found->Output, Found->Shot, Found->Camera, MinD, MaxD, Coverage, ValidVelocity, MaxMotion);
        });
        FlushRenderingCommands();
    }
    return Found;
}
void AttachGuides(FSceneViewFamilyContext &Context, const FMoviePipelineRenderPassMetrics &Metrics,
                  const FString &Pass)
{
    if (!IsFinalFrameRouteActive() || GuideMode == 0 || Metrics.bDiscardResult ||
        Metrics.SpatialSampleIndex != 0 || Metrics.TemporalSampleIndex != 0 ||
        Metrics.TileIndexes != FIntPoint::ZeroValue)
        return;
    Context.ViewExtensions.Add(FSceneViewExtensions::NewExtension<FGuideExtension>(Metrics, Pass, GuideMode));
}
} // namespace DLSS5NRMRQ

void UDLSS5NRMoviePipelineDeferredPass::AddViewExtensions(FSceneViewFamilyContext &Context,
                                                          FMoviePipelineRenderPassMetrics &Metrics)
{
    Super::AddViewExtensions(Context, Metrics);
    DLSS5NRMRQ::AttachGuides(Context, Metrics, TEXT("FinalImage"));
}
void UDLSS5NRMoviePipelinePathTracerPass::AddViewExtensions(FSceneViewFamilyContext &Context,
                                                            FMoviePipelineRenderPassMetrics &Metrics)
{
    Super::AddViewExtensions(Context, Metrics);
    DLSS5NRMRQ::AttachGuides(Context, Metrics, TEXT("PathTracer"));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/DLSS5NRMRQGuideTests.inl"
#endif
