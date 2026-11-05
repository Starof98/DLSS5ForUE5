class FDLSS5NRMRQSeedCS : public FGlobalShader
{
  public:
    DECLARE_GLOBAL_SHADER(FDLSS5NRMRQSeedCS);
    SHADER_USE_PARAMETER_STRUCT(FDLSS5NRMRQSeedCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, TestDepth)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, TestVelocity)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters &P)
    {
        return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
    }
};
IMPLEMENT_GLOBAL_SHADER(FDLSS5NRMRQSeedCS, "/DLSS5NR/Private/DLSS5NRMRQGuides.usf", "SeedMRQGuidesTestCS",
                        SF_Compute);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDLSS5NRMRQGuideGPUTest, "DLSS5.GPU.MRQOutputFrameGuides",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter |
                                     EAutomationTestFlags::NonNullRHI)
bool FDLSS5NRMRQGuideGPUTest::RunTest(const FString &)
{
    for (int32 Case = 0; Case < 6; ++Case)
    {
        TArray<float> DepthResults;
        TArray<FVector2f> MotionResults;
        ENQUEUE_RENDER_COMMAND(
            DLSS5NRMRQGuideTest)([Case, &DepthResults, &MotionResults](FRHICommandListImmediate &Cmd) {
            FRHIGPUTextureReadback DR(TEXT("NRMRQTestDepth")), MR(TEXT("NRMRQTestMotion"));
            FRDGBuilder Graph(Cmd);
            auto Make = [&](EPixelFormat Format, const TCHAR *Name) {
                return Graph.CreateTexture(
                    FRDGTextureDesc::Create2D(FIntPoint(8, 8), Format, FClearValueBinding::Black,
                                              TexCreate_ShaderResource | TexCreate_UAV),
                    Name);
            };
            auto InputD = Make(PF_R32_FLOAT, TEXT("NRTestInputD")),
                 InputV = Make(PF_A32B32G32R32F, TEXT("NRTestInputV"));
            auto *Seed = Graph.AllocParameters<FDLSS5NRMRQSeedCS::FParameters>();
            Seed->TestDepth = Graph.CreateUAV(InputD);
            Seed->TestVelocity = Graph.CreateUAV(InputV);
            TShaderMapRef<FDLSS5NRMRQSeedCS> SeedShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("NR MRQ guide seed"), SeedShader, Seed,
                                         FIntVector(1, 1, 1));
            auto D = Make(PF_R32_FLOAT, TEXT("NRTestOutputD")), M = Make(PF_G32R32F, TEXT("NRTestOutputM"));
            auto *P = Graph.AllocParameters<FDLSS5NRMRQGuidesCS::FParameters>();
            P->SourceDepth = InputD;
            P->SourceVelocity = InputV;
            P->SourceMin = FIntPoint::ZeroValue;
            P->SourceSize = FIntPoint(8, 8);
            P->OutputSize = FIntPoint(8, 8);
            P->ProjectionJitter = Case == 5 ? FVector2f(.25f, 0) : FVector2f::ZeroVector;
            P->OutputClipToPrevious = FMatrix44f::Identity;
            P->EngineClipToPrevious = FMatrix44f::Identity;
            if (Case == 1 || Case == 3)
                P->OutputClipToPrevious.M[3][0] = .25f;
            if (Case == 2)
            {
                P->OutputClipToPrevious.M[0][0] = 2;
                P->OutputClipToPrevious.M[1][1] = 2;
            }
            if (Case == 3)
                P->EngineClipToPrevious.M[3][0] = .2f;
            P->UseEngineVelocity = Case == 3 ? 1 : 0;
            P->ResetMotion = Case == 4 ? 1 : 0;
            P->GuideDepth = Graph.CreateUAV(D);
            P->GuideMotion = Graph.CreateUAV(M);
            TShaderMapRef<FDLSS5NRMRQGuidesCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("NR MRQ guide actual shader"), Shader, P,
                                         FIntVector(1, 1, 1));
            AddEnqueueCopyPass(Graph, &DR, D);
            AddEnqueueCopyPass(Graph, &MR, M);
            Graph.Execute();
            Cmd.SubmitAndBlockUntilGPUIdle();
            if (DR.IsReady() && MR.IsReady())
            {
                int32 DS = 0, MS = 0;
                auto *DP = static_cast<float *>(DR.Lock(DS));
                auto *MP = static_cast<FVector2f *>(MR.Lock(MS));
                if (DP && MP)
                {
                    DepthResults.SetNum(64);
                    MotionResults.SetNum(64);
                    for (int32 Y = 0; Y < 8; ++Y)
                    {
                        FMemory::Memcpy(DepthResults.GetData() + Y * 8, DP + Y * DS, 8 * sizeof(float));
                        FMemory::Memcpy(MotionResults.GetData() + Y * 8, MP + Y * MS, 8 * sizeof(FVector2f));
                    }
                }
                DR.Unlock();
                MR.Unlock();
            }
        });
        FlushRenderingCommands();
        if (!TestEqual(TEXT("GPU guide pixel count"), MotionResults.Num(), 64))
            return false;
        for (int32 Y = 0; Y < 8; ++Y)
            for (int32 X = 0; X < 8; ++X)
            {
                const int32 I = Y * 8 + X;
                const float ExpectedDepth = (FMath::Min(X + (Case == 5 ? 1 : 0), 7) + .5f) / 8;
                FVector2f Expected(0, 0);
                if (Case == 1)
                    Expected.X = 1;
                if (Case == 2)
                    Expected = FVector2f(X + .5f - 4, Y + .5f - 4);
                if (Case == 3)
                    Expected = FVector2f(-.2f, -.8f);
                TestTrue(*FString::Printf(TEXT("depth case=%d pixel=%d"), Case, I),
                         FMath::IsNearlyEqual(DepthResults[I], ExpectedDepth, .0001f));
                TestTrue(*FString::Printf(TEXT("motion sign/zoom/object/reset case=%d pixel=%d"), Case, I),
                         MotionResults[I].Equals(Expected, .002f));
            }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDLSS5NRMRQCameraTest, "DLSS5.MRQ.OutputCameraReprojection",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDLSS5NRMRQCameraTest::RunTest(const FString &)
{
    const FMatrix Identity = FMatrix::Identity;
    FMatrix Previous = Identity;
    Previous.M[0][0] = 2.0;
    Previous.M[1][1] = 2.0;
    const auto Zoom = Identity.Inverse() * Previous;
    const FVector4 P = Zoom.TransformFVector4(FVector4(.25, .25, .5, 1));
    TestTrue(TEXT("Zoom changes reprojection even with stationary camera"),
             FMath::IsNearlyEqual(P.X, .5) && FMath::IsNearlyEqual(P.Y, .5));
    FMoviePipelineRenderPassMetrics A, B;
    A.OutputState.OutputFrameNumber = 42;
    B = A;
    TestEqual(TEXT("Same output matches exact guide key"), DLSS5NRMRQ::FrameKey(A, TEXT("PathTracer")),
              DLSS5NRMRQ::FrameKey(B, TEXT("PathTracer")));
    B.OutputState.CameraIndex++;
    TestNotEqual(TEXT("Cameras isolated"), DLSS5NRMRQ::FrameKey(A, TEXT("PathTracer")),
                 DLSS5NRMRQ::FrameKey(B, TEXT("PathTracer")));
    B = A;
    B.OutputState.ShotIndex++;
    TestNotEqual(TEXT("Shots isolated"), DLSS5NRMRQ::FrameKey(A, TEXT("PathTracer")),
                 DLSS5NRMRQ::FrameKey(B, TEXT("PathTracer")));
    B = A;
    B.OutputState.OutputFrameNumber++;
    TestNotEqual(TEXT("Frames isolated"), DLSS5NRMRQ::FrameKey(A, TEXT("PathTracer")),
                 DLSS5NRMRQ::FrameKey(B, TEXT("PathTracer")));
    TestNotEqual(TEXT("Render paths isolated"), DLSS5NRMRQ::FrameKey(A, TEXT("PathTracer")),
                 DLSS5NRMRQ::FrameKey(A, TEXT("FinalImage")));
    return true;
}
