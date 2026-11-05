#include "DLSS5NRMoviePipelineOutput.h"
#include "DLSS5NRMoviePipelineBridge.h"
#include "DLSS5NRMoviePipelineGuides.h"
#include "DLSS5NRMoviePipelineGuidedPass.h"
#include "DLSS5ForUE5Module.h"
#include "DLSS5NRCVars.h"
#include "DLSS5NRRuntime.h"
#include "MoviePipeline.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineColorSetting.h"
#include "MoviePipelineImageQuantization.h"
#include "MovieRenderPipelineDataTypes.h"
#include "ImagePixelData.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "Misc/AutomationTest.h"
#if DLSS5NR_WITH_D3D12
#include "ID3D12DynamicRHI.h"
#endif

namespace
{
FThreadSafeCounter ActiveFinalRoutes;
uint64 NextStreamId = 1; // Game thread only. Never reused in the process.

const TCHAR *ContractName(EDLSS5NRColorContract Contract)
{
    switch (Contract)
    {
    case EDLSS5NRColorContract::NRStudioPNG8Reference:
        return TEXT("NRStudio_sRGB_PNG8");
    case EDLSS5NRColorContract::MatchSDRsRGB:
        return TEXT("SDR_sRGB_float");
    default:
        return TEXT("LegacyLinear");
    }
}

struct FFinalNRConfig
{
    int32 Passes, Preset, Style;
    float Intensity, Tone, Structure, Skin, HighlightStart, HighlightEnd;
    bool AutoMask, UICorrection, Highlight;
    uint32 Hash() const
    {
        uint32 H = GetTypeHash(Passes);
        const uint32 Values[] = {
            GetTypeHash(Preset),         GetTypeHash(Style),        GetTypeHash(Intensity),
            GetTypeHash(Tone),           GetTypeHash(Structure),    GetTypeHash(Skin),
            GetTypeHash(HighlightStart), GetTypeHash(HighlightEnd), GetTypeHash(AutoMask),
            GetTypeHash(UICorrection),   GetTypeHash(Highlight)};
        for (uint32 V : Values)
            H = HashCombine(H, V);
        return H;
    }
};

float EncodeSDRsRGB(float Linear)
{
    // SDR tonemap viewport targets are bounded UNORM. Never apply this to scene-linear HDR.
    const float X = FMath::Clamp(Linear, 0.0f, 1.0f);
    return X <= 0.0031308f ? 12.92f * X : 1.055f * FMath::Pow(X, 1.0f / 2.4f) - 0.055f;
}
float DecodeSDRsRGB(float Encoded)
{
    const float X = FMath::Max(Encoded, 0.0f);
    return X <= 0.04045f ? X / 12.92f : FMath::Pow((X + 0.055f) / 1.055f, 2.4f);
}

TArray64<FFloat16Color> BuildPNG8Reference(const FImagePixelData *Source, bool bKeepAlpha)
{
    TArray64<FFloat16Color> Prepared;
    TUniquePtr<FImagePixelData> Quantized =
        UE::MoviePipeline::QuantizeImagePixelDataToBitDepth(Source, 8, nullptr, true);
    if (!Quantized || Quantized->GetType() != EImagePixelType::Color)
        return Prepared;
    const auto *Colors = static_cast<const TImagePixelData<FColor> *>(Quantized.Get());
    Prepared.SetNumUninitialized(Colors->Pixels.Num());
    for (int64 I = 0; I < Prepared.Num(); ++I)
    {
        const FColor C = Colors->Pixels[I];
        // FLinearColor(FColor) would silently decode sRGB. These are encoded proxy values.
        Prepared[I] = FFloat16Color(
            FLinearColor(C.R / 255.0f, C.G / 255.0f, C.B / 255.0f, bKeepAlpha ? C.A / 255.0f : 1.0f));
    }
    return Prepared;
}

bool ProcessFinal_RenderThread(FRHICommandListImmediate &RHICmdList, const TArray64<FFloat16Color> &Source,
                               FIntPoint Size, uint64 KeyBase,
                               TSharedPtr<DLSS5NRMRQ::FGuides, ESPMode::ThreadSafe> Guides,
                               const TArray64<FFloat16Color> &PreparedNR, EDLSS5NRColorContract Contract,
                               bool bReferenceParameters, uint32 PreviousConfigHash, uint32 &OutConfigHash,
                               bool &bReset, bool bMatchSRGB, int32 PreviousPassCount, int32 &OutPassCount,
                               TArray<FFloat16Color> &Result)
{
#if !DLSS5NR_WITH_D3D12
    return false;
#else
    if (!IsRHID3D12())
    {
        UE_LOG(LogDLSS5, Error, TEXT("NRFINAL requires D3D12: active RHI is incompatible"));
        return false;
    }
    if (DLSS5NR::GetEnable() == 0)
    {
        UE_LOG(LogDLSS5, Error,
               TEXT("NRFINAL disabled by r.DLSS5.Enable=0; set it to 1 in MRQ Console Variables"));
        return false;
    }
    if (!Guides && DLSS5NR::GetEnableSceneGuides() != 0 && DLSS5NR::GetFeedSceneGuides() != 0)
    {
        UE_LOG(LogDLSS5, Error,
               TEXT("NRFINAL color-only: EnableSceneGuides=1 and FeedSceneGuides=1 are active; set "
                    "r.DLSS5.FeedSceneGuides=0 in the job AND its CVar presets"));
        return false;
    }
    if (Size.X > static_cast<int32>(GMaxTextureDimensions) ||
        Size.Y > static_cast<int32>(GMaxTextureDimensions))
    {
        UE_LOG(LogDLSS5, Error, TEXT("NRFINAL assembled image exceeds GPU texture dimension limit"));
        return false;
    }
    const FFinalNRConfig Config = {FMath::Clamp(DLSS5NR::GetPassCount(), 1, 4),
                                   FMath::Clamp(DLSS5NR::GetPreset(), 0, 3),
                                   DLSS5NR::GetStyle(),
                                   DLSS5NR::GetIntensity(),
                                   DLSS5NR::GetLocalTone(),
                                   bReferenceParameters ? 1.0f : DLSS5NR::GetLocalStructure(),
                                   bReferenceParameters ? 1.0f : DLSS5NR::GetSkinStructure(),
                                   DLSS5NR::GetHighlightStart(),
                                   DLSS5NR::GetHighlightEnd(),
                                   bReferenceParameters || DLSS5NR::GetAutoMask() != 0,
                                   !bReferenceParameters && DLSS5NR::GetUICorrection() != 0,
                                   !bReferenceParameters && DLSS5NR::GetHighlightPreserve() != 0};
    OutPassCount = Config.Passes;
    OutConfigHash = Config.Hash();
    OutConfigHash = HashCombine(OutConfigHash, GetTypeHash(Guides ? (Guides->bEngineVelocity ? 2 : 1) : 0));
    bReset = bReset || PreviousPassCount != OutPassCount || PreviousConfigHash != OutConfigHash ||
             (Guides && Guides->bReset);
    UE_LOG(LogDLSS5, Display,
           TEXT("NRFINAL settings contract=%s profile=%s passes=%d preset=%d style=%d intensity=%.6f "
                "tone=%.6f structure=%.6f skin=%.6f autoMask=%d uiCorrection=%d highlight=%d reset=%d"),
           ContractName(Contract), bReferenceParameters ? TEXT("NRStudio01") : TEXT("PluginCVars"),
           Config.Passes, Config.Preset, Config.Style, Config.Intensity, Config.Tone, Config.Structure,
           Config.Skin, Config.AutoMask ? 1 : 0, Config.UICorrection ? 1 : 0, Config.Highlight ? 1 : 0,
           bReset ? 1 : 0);
    const uint64 RenderFrame = GFrameNumberRenderThread;
    FTextureRHIRef GuideDepth = Guides ? Guides->Depth->GetRHI() : nullptr;
    FTextureRHIRef GuideMotion = Guides ? Guides->Motion->GetRHI() : nullptr;
    const FIntRect Rect(FIntPoint::ZeroValue, Size);
    TArray64<FFloat16Color> EncodedSource;
    const FFloat16Color *Upload = Source.GetData();
    if (Contract == EDLSS5NRColorContract::NRStudioPNG8Reference)
    {
        if (PreparedNR.Num() != Source.Num())
        {
            UE_LOG(LogDLSS5, Error, TEXT("NRFINAL missing PNG8 reference input"));
            return false;
        }
        Upload = PreparedNR.GetData();
    }
    else if (bMatchSRGB)
    {
        EncodedSource.SetNumUninitialized(Source.Num());
        for (int64 I = 0; I < Source.Num(); ++I)
        {
            const FLinearColor C = Source[I].GetFloats();
            EncodedSource[I] =
                FFloat16Color(FLinearColor(EncodeSDRsRGB(C.R), EncodeSDRsRGB(C.G), EncodeSDRsRGB(C.B), C.A));
        }
        Upload = EncodedSource.GetData();
    }
    auto MakeTexture = [&Size, &RHICmdList](const TCHAR *Name, ERHIAccess Access) {
        return RHICmdList.CreateTexture(
            FRHITextureCreateDesc::Create2D(Name, Size, PF_FloatRGBA)
                .SetFlags(TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable)
                .SetInitialState(Access));
    };
    FTextureRHIRef Current = MakeTexture(TEXT("DLSS5NR.FinalAccumulatedInput"), ERHIAccess::CopyDest);
    if (!Current)
        return false;
    uint32 Stride = 0;
    void *Data = RHICmdList.LockTexture2D(Current, 0, RLM_WriteOnly, Stride, false);
    if (!Data)
        return false;
    for (int32 Y = 0; Y < Size.Y; ++Y)
        FMemory::Memcpy(static_cast<uint8 *>(Data) + static_cast<uint64>(Y) * Stride,
                        Upload + static_cast<int64>(Y) * Size.X, Size.X * sizeof(FFloat16Color));
    RHICmdList.UnlockTexture2D(Current, 0, false);
    RHICmdList.Transition(FRHITransitionInfo(Current, ERHIAccess::CopyDest, ERHIAccess::CopySrc));

    for (int32 PassIndex = 0; PassIndex < OutPassCount; ++PassIndex)
    {
        FTextureRHIRef Output = MakeTexture(TEXT("DLSS5NR.FinalAccumulatedOutput"), ERHIAccess::CopyDest);
        if (!Output)
            return false;
        RHICmdList.CopyTexture(Current, Output, FRHICopyTextureInfo());
        RHICmdList.Transition(FRHITransitionInfo(Current, ERHIAccess::CopySrc, ERHIAccess::SRVCompute));
        RHICmdList.Transition(FRHITransitionInfo(Output, ERHIAccess::CopyDest, ERHIAccess::UAVCompute));
        const FGPUFenceRHIRef Fence = RHICreateGPUFence(TEXT("DLSS5NR.FinalFrameComplete"));
        bool bSucceeded = false;
        // Synchronous offline route: references below survive the explicit RHI flush.
        RHICmdList.EnqueueLambda(TEXT("DLSS5NR.FinalFrameEvaluate"),
                                 [Current, Output, GuideDepth, GuideMotion, Guides, Fence, KeyBase, PassIndex,
                                  RenderFrame, Rect, bReset, Config, &bSucceeded](FRHICommandListBase &Cmd) {
                                     ID3D12DynamicRHI *RHI = GetID3D12DynamicRHI();
                                     if (!RHI)
                                         return;
                                     const uint32 DeviceIndex = RHI->RHIGetResourceDeviceIndex(Output);
                                     auto *NativeCmd = RHI->RHIGetGraphicsCommandList(Cmd, DeviceIndex);
                                     auto *Device = RHI->RHIGetDevice(DeviceIndex);
                                     if (!NativeCmd || !Device)
                                         return;
                                     RHI->RHIUpdateResourceResidency(Cmd, DeviceIndex, Current);
                                     RHI->RHIUpdateResourceResidency(Cmd, DeviceIndex, Output);
                                     if (GuideDepth)
                                         RHI->RHIUpdateResourceResidency(Cmd, DeviceIndex, GuideDepth);
                                     if (GuideMotion)
                                         RHI->RHIUpdateResourceResidency(Cmd, DeviceIndex, GuideMotion);
                                     RHI->RHIFlushResourceBarriers(Cmd, DeviceIndex);
                                     FDLSS5NRRuntime &Runtime = FDLSS5NRRuntime::Get();
                                     FScopeLock Lock(&Runtime.GetExecutionMutex());
                                     if (!Runtime.EnsureInitialized(Device))
                                         return;
                                     FDLSS5NREvaluateDesc D;
                                     D.HistoryKey = KeyBase + PassIndex;
                                     D.FrameNumber = RenderFrame;
                                     D.bOfflineFinalFrame = true;
                                     D.CompletionFence = Fence;
                                     D.CommandList = NativeCmd;
                                     D.Color = RHI->RHIGetResource(Current);
                                     D.Output = RHI->RHIGetResource(Output);
                                     D.ColorRect = Rect;
                                     D.OutputRect = Rect;
                                     D.bReset = bReset;
                                     D.Depth = GuideDepth ? RHI->RHIGetResource(GuideDepth) : nullptr;
                                     D.MotionVectors =
                                         GuideMotion ? RHI->RHIGetResource(GuideMotion) : nullptr;
                                     D.DepthRect = GuideDepth ? Rect : FIntRect();
                                     D.MotionRect = GuideMotion ? Rect : FIntRect();
                                     D.bDepthInverted = true;
                                     D.MotionScaleX = 1;
                                     D.MotionScaleY = 1;
                                     D.PreExposure = Guides ? Guides->PreExposure : 1;
                                     D.Preset = Config.Preset;
                                     D.Intensity = Config.Intensity;
                                     D.LocalTone = Config.Tone;
                                     D.LocalStructure = Config.Structure;
                                     D.SkinStructure = Config.Skin;
                                     D.bAutoMask = Config.AutoMask;
                                     D.Style = Config.Style;
                                     D.bUICorrection = Config.UICorrection;
                                     bSucceeded = Runtime.Evaluate(D);
                                     RHI->RHIFinishExternalComputeWork(Cmd, DeviceIndex, NativeCmd);
                                 });
        RHICmdList.WriteGPUFence(Fence);
        RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
        // Wait before releasing per-pass resources, including on failed Evaluate.
        RHICmdList.SubmitAndBlockUntilGPUIdle();
        if (!bSucceeded)
            return false;
        RHICmdList.Transition(FRHITransitionInfo(Output, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
        Current = Output;
    }
    FReadSurfaceDataFlags ReadFlags(RCM_MinMax);
    ReadFlags.SetLinearToGamma(false);
    RHICmdList.ReadSurfaceFloatData(Current, Rect, Result, ReadFlags);
    if (Result.Num() != static_cast<int64>(Size.X) * Size.Y)
        return false;
    const bool bHighlight = Config.Highlight;
    const float Start = Config.HighlightStart, End = Config.HighlightEnd;
    for (int32 I = 0; I < Result.Num(); ++I)
    {
        const FLinearColor S = Upload[I].GetFloats();
        FLinearColor N = Result[I].GetFloats();
        if (bHighlight && End > Start)
        {
            float T = FMath::Clamp((FMath::Max3(S.R, S.G, S.B) - Start) / (End - Start), 0.0f, 1.0f);
            T = T * T * (3.0f - 2.0f * T);
            N.R = FMath::Lerp(N.R, S.R, T);
            N.G = FMath::Lerp(N.G, S.G, T);
            N.B = FMath::Lerp(N.B, S.B, T);
        }
        if (bMatchSRGB)
        {
            N.R = DecodeSDRsRGB(N.R);
            N.G = DecodeSDRsRGB(N.G);
            N.B = DecodeSDRsRGB(N.B);
        }
        N.A = Source[I].A.GetFloat(); // Preserve original alpha, not its PNG8 reference quantization.
        if (!FMath::IsFinite(N.R) || !FMath::IsFinite(N.G) || !FMath::IsFinite(N.B))
            return false;
        Result[I] = FFloat16Color(N);
    }
    return true;
#endif
}
} // namespace

bool DLSS5NRMRQ::IsFinalFrameRouteActive()
{
    return ActiveFinalRoutes.GetValue() > 0;
}

UDLSS5NRMoviePipelineOutput::UDLSS5NRMoviePipelineOutput()
{
    OutputFormat = EImageFormat::EXR;
}

void UDLSS5NRMoviePipelineOutput::SetupForPipelineImpl(UMoviePipeline *Pipeline)
{
    Super::SetupForPipelineImpl(Pipeline);
    Streams.Reset();
    auto Fail = [Pipeline](const TCHAR *Message) {
        UE_LOG(LogDLSS5, Error, TEXT("NRFINAL setup failed: %s"), Message);
        Pipeline->RequestShutdown(true);
    };
    if (!IsRHID3D12())
    {
        Fail(TEXT("D3D12 required"));
        return;
    }
    const auto *Config = Pipeline->GetPipelinePrimaryConfig();
    if (const auto *Color = Config->FindSetting<UMoviePipelineColorSetting>())
    {
        if (Color->bDisableToneCurve || Color->OCIOConfiguration.bIsEnabled)
        {
            Fail(TEXT("Use Tone Curve ON and OCIO OFF for the post-tonemap NR contract"));
            return;
        }
    }
    for (auto *Output : Config->FindSettings<UMoviePipelineOutputBase>())
    {
        if (Output != this)
        {
            Fail(TEXT("Use this as the only output container; remove PNG/EXR/video/audio outputs for this "
                      "experimental route"));
            return;
        }
    }
    if (ActiveFinalRoutes.Increment() != 1)
    {
        ActiveFinalRoutes.Decrement();
        Fail(TEXT("Concurrent final-frame MRQ routes unsupported"));
        return;
    }
    bOwnsOfflineRoute = true;
    DLSS5NRMRQ::ClearGuides();
    DLSS5NRMRQ::SetGuideMode(static_cast<int32>(GuideMode));
    if (GuideMode != EDLSS5NRMRQGuideMode::Disabled)
    {
        const auto Passes = Config->FindSettings<UMoviePipelineDeferredPassBase>();
        if (Passes.Num() != 1 || (!Passes[0]->IsA<UDLSS5NRMoviePipelineDeferredPass>() &&
                                  !Passes[0]->IsA<UDLSS5NRMoviePipelinePathTracerPass>()))
        {
            Fail(TEXT("Select exactly one DLSS5 NR - Deferred OR DLSS5 NR - Path Tracing under Rendering; "
                      "remove the standard render pass"));
            return;
        }
        const auto *P = Passes[0];
        if (!P->bRenderMainPass || P->bAddDefaultLayer || P->ActorLayers.Num() || P->DataLayers.Num() ||
            !P->bDisableMultisampleEffects)
        {
            Fail(TEXT("MRQ guides requires Render Main Pass ON, stencil layers OFF and Disable Multisample "
                      "Effects ON"));
            return;
        }
    }
    UE_LOG(LogDLSS5, Display,
           TEXT("NRGUIDES setup mode=%d; no world-geometry/offscreen-light input; temporal=1 and tiles=1 "
                "required"),
           static_cast<int32>(GuideMode));
    UE_LOG(LogDLSS5, Display,
           TEXT("NRFINAL active: offline per-sample NR suppressed; final CPU accumulation via GPU roundtrip. "
                "contract=%s profile=%s; guides optional; PNG/EXR; legacy MRQ."),
           ContractName(ColorContract),
           bUseNRStudioReferenceParameters ? TEXT("NRStudio01") : TEXT("PluginCVars"));
}

void UDLSS5NRMoviePipelineOutput::TeardownForPipelineImpl(UMoviePipeline *Pipeline)
{
    FlushRenderingCommands();
    if (bOwnsOfflineRoute)
    {
        DLSS5NRMRQ::SetGuideMode(0);
        DLSS5NRMRQ::ClearGuides();
        ActiveFinalRoutes.Decrement();
        bOwnsOfflineRoute = false;
    }
    Streams.Reset();
    Super::TeardownForPipelineImpl(Pipeline);
}

void UDLSS5NRMoviePipelineOutput::OnReceiveImageDataImpl(FMoviePipelineMergerOutputFrame *Frame)
{
    check(IsInGameThread());
    if (!Frame || !bOwnsOfflineRoute)
    {
        GetPipeline()->RequestShutdown(true);
        return;
    }
    OutputFormat = FileFormat == EDLSS5NRFinalImageFormat::PNG ? EImageFormat::PNG : EImageFormat::EXR;
    if (!bApplyNeuralRendering)
    {
        if (GuideMode != EDLSS5NRMRQGuideMode::Disabled)
        {
            FlushRenderingCommands();
            int32 GuideCount = 0;
            for (const auto &Pair : Frame->ImageOutputData)
            {
                if (Pair.Key.Name != TEXT("FinalImage") && Pair.Key.Name != TEXT("PathTracer"))
                    continue;
                const auto *P = Pair.Value->GetPayload<FImagePixelDataPayload>();
                auto G = P ? DLSS5NRMRQ::TakeGuides(P->SampleState, Pair.Key.Name) : nullptr;
                if (!G || !G->Error.IsEmpty() || !G->Depth || !G->Motion || G->Size != Pair.Value->GetSize())
                {
                    UE_LOG(LogDLSS5, Error,
                           TEXT("NRGUIDES baseline validation failed: %s guide=%dx%d image=%dx%d depth=%d "
                                "motion=%d; match camera aspect ratio to output (no letterboxing)"),
                           G ? *G->Error : TEXT("missing exact-frame guides"), G ? G->Size.X : 0,
                           G ? G->Size.Y : 0, Pair.Value->GetSize().X, Pair.Value->GetSize().Y,
                           G && G->Depth ? 1 : 0, G && G->Motion ? 1 : 0);
                    GetPipeline()->RequestShutdown(true);
                    return;
                }
                UE_LOG(
                    LogDLSS5, Display,
                    TEXT("NRGUIDES baseline paired output=%d shot=%d camera=%d depth=YES motion=YES mode=%d"),
                    G->Output, G->Shot, G->Camera, static_cast<int32>(GuideMode));
                ++GuideCount;
            }
            if (GuideCount == 0)
            {
                GetPipeline()->RequestShutdown(true);
                return;
            }
        }
        Streams.Reset();
        UE_LOG(LogDLSS5, Display, TEXT("NRFINAL baseline: per-sample and final NR bypassed"));
        Super::OnReceiveImageDataImpl(Frame);
        return;
    }
    // Clone ALL data so no other observer sees a mutated merger frame.
    FMoviePipelineMergerOutputFrame Processed;
    Processed.FrameOutputState = Frame->FrameOutputState;
    Processed.ExpectedRenderPasses = Frame->ExpectedRenderPasses;
    int32 ProcessedCount = 0;
    for (const auto &Pair : Frame->ImageOutputData)
    {
        if (Pair.Key.Name != TEXT("FinalImage") && Pair.Key.Name != TEXT("PathTracer"))
        {
            Processed.ImageOutputData.Add(Pair.Key, Pair.Value->CopyImageData());
            continue;
        }
        const auto *Payload = Pair.Value->GetPayload<FImagePixelDataPayload>();
        const FIntPoint Size = Pair.Value->GetSize();
        const int64 Count = static_cast<int64>(Size.X) * Size.Y;
        if (!Payload || Size.X <= 0 || Size.Y <= 0 || Count > MAX_int32 || !Pair.Value->IsDataWellFormed())
        {
            UE_LOG(LogDLSS5, Error, TEXT("NRFINAL invalid accumulated image"));
            GetPipeline()->RequestShutdown(true);
            return;
        }
        TArray64<FFloat16Color> Source;
        Source.SetNumUninitialized(Count);
        // FColor may have already lost HDR precision. Reject rather than guess its encoding.
        if (Pair.Value->GetType() == EImagePixelType::Float16)
        {
            const auto *P = static_cast<const TImagePixelData<FFloat16Color> *>(Pair.Value.Get());
            FMemory::Memcpy(Source.GetData(), P->Pixels.GetData(), Count * sizeof(FFloat16Color));
        }
        else if (Pair.Value->GetType() == EImagePixelType::Float32)
        {
            const auto *P = static_cast<const TImagePixelData<FLinearColor> *>(Pair.Value.Get());
            for (int64 I = 0; I < Count; ++I)
                Source[I] = FFloat16Color(P->Pixels[I]);
        }
        else
        {
            UE_LOG(LogDLSS5, Error, TEXT("NRFINAL requires float accumulated input (8-bit input rejected)"));
            GetPipeline()->RequestShutdown(true);
            return;
        }
        for (const auto &P : Source)
        {
            const FLinearColor C = P.GetFloats();
            if (!FMath::IsFinite(C.R) || !FMath::IsFinite(C.G) || !FMath::IsFinite(C.B))
            {
                UE_LOG(LogDLSS5, Error, TEXT("NRFINAL non-finite/FP16 overflow input"));
                GetPipeline()->RequestShutdown(true);
                return;
            }
        }
        TArray64<FFloat16Color> PreparedNR;
        if (ColorContract == EDLSS5NRColorContract::NRStudioPNG8Reference)
        {
            // Use Epic's actual PNG quantizer, including its deterministic dither and the
            // original float type. The same quantizer is used to write NRInput_* PNG.
            PreparedNR =
                BuildPNG8Reference(Pair.Value.Get(), bWriteAlpha || Payload->bRequireTransparentOutput);
            if (PreparedNR.Num() != Count)
            {
                UE_LOG(LogDLSS5, Error, TEXT("NRFINAL PNG8 reference quantization failed"));
                GetPipeline()->RequestShutdown(true);
                return;
            }
        }
        const int32 Shot = Payload->SampleState.OutputState.ShotIndex;
        const int32 Output = Payload->SampleState.OutputState.OutputFrameNumber;
        const FString Stream = FString::Printf(TEXT("%d|%s|%s"), Shot, *Pair.Key.CameraName, *Pair.Key.Name);
        FStreamState &State = Streams.FindOrAdd(Stream);
        if (State.KeyBase == 0)
        {
            // Namespace outside the normal view/pass keys (normal pass indices are 0..3).
            State.KeyBase = (NextStreamId++ << 32) | 0x80000000ULL;
        }
        const bool bMatchSRGB = ColorContract != EDLSS5NRColorContract::LegacyLinear;
        bool bReset = State.LastOutput == MIN_int32 ||
                      static_cast<int64>(Output) != static_cast<int64>(State.LastOutput) + 1 ||
                      State.Size != Size || Payload->SampleState.bCameraCut ||
                      State.LastColorContract != static_cast<int32>(ColorContract) || bResetEveryOutputFrame;
        TSharedPtr<DLSS5NRMRQ::FGuides, ESPMode::ThreadSafe> Guides;
        if (GuideMode != EDLSS5NRMRQGuideMode::Disabled)
        {
            FlushRenderingCommands();
            Guides = DLSS5NRMRQ::TakeGuides(Payload->SampleState, Pair.Key.Name);
            if (!Guides || !Guides->Error.IsEmpty() || !Guides->Depth || !Guides->Motion ||
                Guides->Size != Size)
            {
                UE_LOG(LogDLSS5, Error,
                       TEXT("NRGUIDES output=%d shot=%d camera=%d rejected: %s guide=%dx%d image=%dx%d "
                            "depth=%d motion=%d; require matching camera/output aspect and exact-frame "
                            "guides; no color-only fallback"),
                       Output, Shot, Payload->SampleState.OutputState.CameraIndex,
                       Guides ? *Guides->Error : TEXT("missing guides: check guided render pass and Tonemap"),
                       Guides ? Guides->Size.X : 0, Guides ? Guides->Size.Y : 0, Size.X, Size.Y,
                       Guides && Guides->Depth ? 1 : 0, Guides && Guides->Motion ? 1 : 0);
                GetPipeline()->RequestShutdown(true);
                return;
            }
            UE_LOG(LogDLSS5, Display,
                   TEXT("NRGUIDES paired output=%d shot=%d camera=%d depth=YES motion=YES mode=%d "
                        "preExposure=%.6f fov=%.6f"),
                   Output, Shot, Guides->Camera, static_cast<int32>(GuideMode), Guides->PreExposure,
                   Guides->FOV);
        }
        TArray<FFloat16Color> Pixels;
        bool bSuccess = false;
        int32 PassCount = 0;
        uint32 ConfigHash = 0;
        ENQUEUE_RENDER_COMMAND(DLSS5NRProcessAccumulatedFrame)(
            [&Source, &PreparedNR, Guides, Size, Key = State.KeyBase, &bReset, bMatchSRGB,
             Contract = ColorContract, ReferenceParameters = bUseNRStudioReferenceParameters,
             PreviousConfigHash = State.ConfigHash, &ConfigHash, PreviousPassCount = State.LastPassCount,
             &Pixels, &bSuccess, &PassCount](FRHICommandListImmediate &Cmd) {
                bSuccess = ProcessFinal_RenderThread(
                    Cmd, Source, Size, Key, Guides, PreparedNR, Contract, ReferenceParameters,
                    PreviousConfigHash, ConfigHash, bReset, bMatchSRGB, PreviousPassCount, PassCount, Pixels);
            });
        FlushRenderingCommands();
        if (!bSuccess)
        {
            UE_LOG(LogDLSS5, Error, TEXT("NRFINAL evaluation failed: no raw fallback file is written"));
            GetPipeline()->RequestShutdown(true);
            return;
        }
        auto Data = Pair.Value->CopyImageData(); // Preserve MRQ payload and original float type.
        if (Data->GetType() == EImagePixelType::Float16)
            FMemory::Memcpy(static_cast<TImagePixelData<FFloat16Color> *>(Data.Get())->Pixels.GetData(),
                            Pixels.GetData(), Count * sizeof(FFloat16Color));
        else
        {
            auto *Dest = static_cast<TImagePixelData<FLinearColor> *>(Data.Get());
            for (int64 I = 0; I < Count; ++I)
            {
                const float OriginalAlpha = Dest->Pixels[I].A;
                Dest->Pixels[I] = Pixels[I].GetFloats();
                Dest->Pixels[I].A = OriginalAlpha;
            }
        }
        State.LastOutput = Output;
        State.Size = Size;
        State.LastPassCount = PassCount;
        State.bLastSRGB = bMatchSRGB;
        State.LastColorContract = static_cast<int32>(ColorContract);
        State.ConfigHash = ConfigHash;
        if (bWriteInputReference)
        {
            FMoviePipelinePassIdentifier InputId = Pair.Key;
            InputId.Name = TEXT("NRInput_") + Pair.Key.Name;
            Processed.ExpectedRenderPasses.Add(InputId);
            Processed.ImageOutputData.Add(InputId, Pair.Value->CopyImageData());
        }
        UE_LOG(LogDLSS5, Display,
               TEXT("NRFINAL output=%d shot=%d camera=%s width=%d height=%d passes=%d reset=%d contract=%s "
                    "stage=after_accumulation success=1"),
               Output, Shot, *Pair.Key.CameraName, Size.X, Size.Y, PassCount, bReset ? 1 : 0,
               ContractName(ColorContract));
        Processed.ImageOutputData.Add(Pair.Key, MoveTemp(Data));
        ++ProcessedCount;
    }
    if (ProcessedCount == 0)
    {
        UE_LOG(LogDLSS5, Error,
               TEXT("NRFINAL no FinalImage pass: use Deferred Rendering or Path Tracer, not render "
                    "graph/AOV-only"));
        GetPipeline()->RequestShutdown(true);
        return;
    }
    OutputFormat = FileFormat == EDLSS5NRFinalImageFormat::PNG ? EImageFormat::PNG : EImageFormat::EXR;
    Super::OnReceiveImageDataImpl(&Processed);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/DLSS5NRMRQColorTests.inl"
#endif
