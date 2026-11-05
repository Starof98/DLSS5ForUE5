#pragma once

#include "CoreMinimal.h"
#include "MoviePipelineImageSequenceOutput.h"
#include "DLSS5NRMoviePipelineOutput.generated.h"

UENUM(BlueprintType)
enum class EDLSS5NRFinalImageFormat : uint8
{
    PNG,
    EXR
};

UENUM(BlueprintType)
enum class EDLSS5NRColorContract : uint8
{
    MatchSDRsRGB,
    LegacyLinear,
    NRStudioPNG8Reference UMETA(DisplayName = "NRStudio sRGB - PNG 8-bit reference")
};

UENUM(BlueprintType)
enum class EDLSS5NRMRQGuideMode : uint8
{
    Disabled,
    CameraDepth UMETA(DisplayName = "Depth + Camera Motion (static objects)"),
    EngineMotion UMETA(DisplayName = "Depth + Object Motion")
};

// MRQ legacy output container. Deferred and Path Tracing both feed the merger.
// Processing is AFTER CPU accumulation, BEFORE output quantization/compositing.
UCLASS(BlueprintType)
class DLSS5FORUE5_NVNGX_API UDLSS5NRMoviePipelineOutput : public UMoviePipelineImageSequenceOutputBase
{
    GENERATED_BODY()
  public:
    UDLSS5NRMoviePipelineOutput();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR")
    EDLSS5NRFinalImageFormat FileFormat = EDLSS5NRFinalImageFormat::EXR;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR")
    bool bWriteAlpha = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR")
    bool bApplyNeuralRendering = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR|Diagnostics", AdvancedDisplay)
    bool bWriteInputReference = false;
    // Explicit reference contract; not a saturation/intensity compensation.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR|Compatibility", AdvancedDisplay)
    EDLSS5NRColorContract ColorContract = EDLSS5NRColorContract::NRStudioPNG8Reference;
    // NRStudio 0.1 uses AutoMask=1, UICorrection=0, structure strengths=1 and no highlight recovery.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR|Compatibility", AdvancedDisplay)
    bool bUseNRStudioReferenceParameters = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR|Diagnostics", AdvancedDisplay,
              meta = (DisplayName = "Reset every output frame"))
    bool bResetEveryOutputFrame = false;
    // Disabled preserves existing R3 presets. Select a guided rendering pass to use these modes.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DLSS5 NR",
              meta = (DisplayName = "Final Frame Guides"))
    EDLSS5NRMRQGuideMode GuideMode = EDLSS5NRMRQGuideMode::Disabled;
    virtual bool IsAlphaAllowed() const override
    {
        return bWriteAlpha;
    }
    virtual void OnReceiveImageDataImpl(FMoviePipelineMergerOutputFrame *InFrame) override;
#if WITH_EDITOR
    virtual FText GetDisplayText() const override
    {
        return NSLOCTEXT("DLSS5", "NRFinalOutput", "DLSS5 NR - Output");
    }
#endif
  protected:
    virtual void SetupForPipelineImpl(UMoviePipeline *InPipeline) override;
    virtual void TeardownForPipelineImpl(UMoviePipeline *InPipeline) override;

  private:
    struct FStreamState
    {
        uint64 KeyBase = 0;
        int32 LastOutput = MIN_int32;
        FIntPoint Size = FIntPoint::ZeroValue;
        int32 LastPassCount = 0;
        bool bLastSRGB = false;
        int32 LastColorContract = -1;
        uint32 ConfigHash = 0;
    };
    TMap<FString, FStreamState> Streams;
    bool bOwnsOfflineRoute = false;
};
