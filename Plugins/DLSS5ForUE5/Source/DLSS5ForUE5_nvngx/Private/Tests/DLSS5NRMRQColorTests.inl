IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDLSS5NRPNG8ReferenceTest, "DLSS5.MRQ.NRStudioPNG8Contract",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDLSS5NRPNG8ReferenceTest::RunTest(const FString &)
{
    TImagePixelData<FFloat16Color> FloatImage(FIntPoint(8, 1));
    const float Values[] = {-0.1f, 0.0f, 0.0031308f, 0.018f, 0.18f, 0.5f, 1.0f, 1.5f};
    for (float V : Values)
        FloatImage.Pixels.Add(FFloat16Color(FLinearColor(V, V, V, 0.3f)));
    auto A = UE::MoviePipeline::QuantizeImagePixelDataToBitDepth(&FloatImage, 8, nullptr, true);
    auto B = UE::MoviePipeline::QuantizeImagePixelDataToBitDepth(&FloatImage, 8, nullptr, true);
    const auto *BytesA = static_cast<const TImagePixelData<FColor> *>(A.Get());
    const auto *BytesB = static_cast<const TImagePixelData<FColor> *>(B.Get());
    auto Proxy = BuildPNG8Reference(&FloatImage, true);
    TestEqual(TEXT("PNG reference pixel count"), Proxy.Num(), int64(8));
    for (int32 I = 0; I < 8; ++I)
    {
        TestTrue(TEXT("Epic PNG dither is reproducible for reference and writer"),
                 BytesA->Pixels[I] == BytesB->Pixels[I]);
        TestEqual(TEXT("Encoded PNG byte enters FP16 without sRGB decoding"), Proxy[I].R.Encoded,
                  FFloat16(BytesA->Pixels[I].R / 255.0f).Encoded);
    }
    TestEqual(TEXT("SDR reference clips negative input"), BytesA->Pixels[0].R, uint8(0));
    TestEqual(TEXT("SDR reference clips above one"), BytesA->Pixels[7].R, uint8(255));
    auto Opaque = BuildPNG8Reference(&FloatImage, false);
    TestEqual(TEXT("Writer's opaque alpha is mirrored in reference input"), Opaque[0].A.GetFloat(), 1.0f);
    TImagePixelData<FColor> Ramp(FIntPoint(256, 1));
    for (int32 I = 0; I < 256; ++I)
        Ramp.Pixels.Add(FColor(I, 128, 255 - I, 255));
    auto RampProxy = BuildPNG8Reference(&Ramp, true);
    for (int32 I = 0; I < 256; ++I)
    {
        TestEqual(TEXT("Full PNG ramp retains encoded values"), RampProxy[I].R.Encoded,
                  FFloat16(I / 255.0f).Encoded);
        const float Encoded = I / 255.0f;
        TestTrue(TEXT("Decode then final writer encode is neutral without NR"),
                 FMath::Abs(EncodeSDRsRGB(DecodeSDRsRGB(Encoded)) - Encoded) < 0.00001f);
    }
    FFinalNRConfig Config = {2, 1, 1, 1, 1, 1, 1, 0.7f, 0.9f, true, false, false};
    uint32 Before = Config.Hash();
    Config.UICorrection = true;
    TestTrue(TEXT("Changing effective NR parameters invalidates history fingerprint"),
             Before != Config.Hash());
    return true;
}
