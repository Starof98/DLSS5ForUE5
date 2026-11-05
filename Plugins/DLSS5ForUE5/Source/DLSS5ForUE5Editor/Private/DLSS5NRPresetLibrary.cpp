#include "DLSS5NRPresetLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "DLSS5NRMoviePipelineGuidedPass.h"
#include "DLSS5NRMoviePipelineOutput.h"
#include "MoviePipelineAntiAliasingSetting.h"
#include "MoviePipelineColorSetting.h"
#include "MoviePipelineConsoleVariableSetting.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePrimaryConfig.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
template <typename T> T *AddSetting(UMoviePipelinePrimaryConfig *Config)
{
    return CastChecked<T>(Config->FindOrAddSettingByClass(T::StaticClass()));
}

// This is the single place to change the defaults offered by the editor menu.
// Called only on a fresh config so it cannot remove or replace user settings.
void ConfigureNewPreset(UMoviePipelinePrimaryConfig *Config, bool bPathTracing)
{
    UMoviePipelineDeferredPassBase *Render = nullptr;
    if (bPathTracing)
    {
        auto *PathTracer = AddSetting<UDLSS5NRMoviePipelinePathTracerPass>(Config);
        PathTracer->bReferenceMotionBlur = false;
        Render = PathTracer;
    }
    else
    {
        Render = AddSetting<UDLSS5NRMoviePipelineDeferredPass>(Config);
    }
    Render->bDisableMultisampleEffects = true;
    Render->bRenderMainPass = true;
    Render->bAddDefaultLayer = false;

    auto *AA = AddSetting<UMoviePipelineAntiAliasingSetting>(Config);
    AA->SpatialSampleCount = 16;
    AA->TemporalSampleCount = 1;
    AA->bOverrideAntiAliasing = true;
    AA->AntiAliasingMethod = AAM_None;

    auto *NR = AddSetting<UDLSS5NRMoviePipelineOutput>(Config);
    NR->FileFormat = EDLSS5NRFinalImageFormat::PNG;
    NR->GuideMode = EDLSS5NRMRQGuideMode::EngineMotion;
    NR->ColorContract = EDLSS5NRColorContract::NRStudioPNG8Reference;
    NR->bApplyNeuralRendering = true;
    NR->bUseNRStudioReferenceParameters = true;
    NR->bResetEveryOutputFrame = false;

    auto *Color = AddSetting<UMoviePipelineColorSetting>(Config);
    Color->bDisableToneCurve = false;
    Color->OCIOConfiguration.bIsEnabled = false;

    auto *CVars = AddSetting<UMoviePipelineConsoleVariableSetting>(Config);
    const TPair<const TCHAR *, float> Values[] = {{TEXT("r.DLSS5.Enable"), 1},
                                                  {TEXT("r.DLSS5.PassCount"), 2},
                                                  {TEXT("r.DLSS5.Preset"), 1},
                                                  {TEXT("r.DLSS5.Style"), 1},
                                                  {TEXT("r.DLSS5.Intensity"), 1},
                                                  {TEXT("r.DLSS5.LocalTone"), 1},
                                                  {TEXT("r.DLSS5.EnableSceneGuides"), 0},
                                                  {TEXT("r.DLSS5.FeedSceneGuides"), 0},
                                                  {TEXT("r.ScreenPercentage"), 100}};
    for (const auto &Entry : Values)
    {
        CVars->AddOrUpdateConsoleVariable(Entry.Key, Entry.Value);
    }
}
} // namespace

UMoviePipelinePrimaryConfig *UDLSS5NRPresetLibrary::CreateMRQPreset(bool bPathTracing,
                                                                    bool bOpenInContentBrowser)
{
    FString PackageName, AssetName;
    const FString Base =
        bPathTracing ? TEXT("/Game/DLSS5/Presets/NR_PathTracing") : TEXT("/Game/DLSS5/Presets/NR_Deferred");
    auto &AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    AssetTools.CreateUniqueAssetName(Base, TEXT(""), PackageName, AssetName);
    UPackage *Package = CreatePackage(*PackageName);
    auto *Config = NewObject<UMoviePipelinePrimaryConfig>(Package, *AssetName, RF_Public | RF_Standalone);
    ConfigureNewPreset(Config, bPathTracing);
    FAssetRegistryModule::AssetCreated(Config);
    Config->MarkPackageDirty();
    if (bOpenInContentBrowser)
    {
        TArray<UObject *> Assets{Config};
        FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"))
            .Get()
            .SyncBrowserToAssets(Assets);
    }
    return Config;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDLSS5NRPresetTest, "DLSS5.MRQ.PresetDefaults",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDLSS5NRPresetTest::RunTest(const FString &)
{
    for (bool bPathTracing : {false, true})
    {
        auto *Config = NewObject<UMoviePipelinePrimaryConfig>();
        ConfigureNewPreset(Config, bPathTracing);
        const auto Passes = Config->FindSettings<UMoviePipelineDeferredPassBase>();
        TestEqual(TEXT("Exactly one compatible render pass"), Passes.Num(), 1);
        if (Passes.Num() != 1)
            return false;
        TestTrue(TEXT("Correct guided render class"),
                 bPathTracing ? Passes[0]->IsA<UDLSS5NRMoviePipelinePathTracerPass>()
                              : Passes[0]->IsA<UDLSS5NRMoviePipelineDeferredPass>());
        TestTrue(TEXT("Multisample effects disabled"), Passes[0]->bDisableMultisampleEffects);
        const auto *AA = Config->FindSetting<UMoviePipelineAntiAliasingSetting>();
        TestEqual(TEXT("Temporal samples aligned with guides"), AA->TemporalSampleCount, 1);
        TestTrue(TEXT("Explicit AA None"), AA->bOverrideAntiAliasing && AA->AntiAliasingMethod == AAM_None);
        const auto *NR = Config->FindSetting<UDLSS5NRMoviePipelineOutput>();
        TestTrue(TEXT("Engine motion and reference color enabled"),
                 NR->GuideMode == EDLSS5NRMRQGuideMode::EngineMotion &&
                     NR->ColorContract == EDLSS5NRColorContract::NRStudioPNG8Reference);
        TestTrue(TEXT("History retained between outputs"), !NR->bResetEveryOutputFrame);
        const auto Entries =
            Config->FindSetting<UMoviePipelineConsoleVariableSetting>()->GetConsoleVariables();
        TestEqual(TEXT("Required CVar overrides included"), Entries.Num(), 9);
    }
    return true;
}
#endif
