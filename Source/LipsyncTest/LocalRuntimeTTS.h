// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RuntimeAudioImporterTypes.h"
#include "UObject/Object.h"
#include "LocalRuntimeTTS.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnRuntimeTTSSpeechResult, const FString&, Text, const TArray<uint8>&, AudioData, int32, SampleRate, int32, NumChannels, ERuntimeRAWAudioFormat, RawFormat);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeTTSError, const FString&, Error);

UCLASS(BlueprintType)
class LIPSYNCTEST_API ULocalRuntimeTTS : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	FString PiperExePath = TEXT("Piper/piper.exe");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	FString ModelPath = TEXT("Models/tts/ru_RU-denis-medium.onnx");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	FString ModelConfigPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	bool bUseCuda = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	int32 SpeakerId = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	float LengthScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	float NoiseScale = 0.667f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	float NoiseW = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	bool bQueueSpeech = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	bool bCancelPreviousOnNewRequest = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	bool bForceWavOutput = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Debug")
	bool bSaveWavDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Debug")
	FString DebugWavDir;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Validation")
	bool bValidatePcm = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Validation")
	bool bLogPcmStats = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Validation", meta = (ClampMin = "0"))
	int32 MinPcmBytes = 4096;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Validation", meta = (ClampMin = "0.0"))
	float BadPcmRmsThreshold = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Validation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BadPcmClipRatioThreshold = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Validation")
	bool bFallbackToCpuOnBadPcm = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS|Validation", meta = (ClampMin = "0"))
	int32 MaxBadPcmRetries = 1;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS", meta = (DisplayName = "OnSpeechResult"))
	FOnRuntimeTTSSpeechResult OnSpeechResult;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS")
	FOnRuntimeTTSError OnError;

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Create Runtime TTS"), Category = "Runtime TTS")
	static ULocalRuntimeTTS* CreateRuntimeTTS();

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Text To Speech"), Category = "Runtime TTS")
	void TextToSpeech(const FString& Text);

private:
	TArray<FString> PendingTexts;
	int32 SpeakGeneration = 0;
	bool bIsGenerating = false;

	FString ResolvePath(const FString& Path) const;
	void EmitError(const FString& Message) const;
	bool LoadSampleRate(int32& OutSampleRate, FString& OutError) const;
	bool RunPiperToRawPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const;
	bool RunPiperToRawPcmWithCuda(const FString& Text, bool bUseCudaOverride, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const;
	bool RunPiperToWavPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const;
	bool ParseWavToPcm16(const TArray<uint8>& WavData, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const;
	bool WritePcm16ToWavFile(const FString& FilePath, const TArray<uint8>& PcmData, int32 SampleRate, int32 NumChannels, FString& OutError) const;
	bool ComputePcmStats(const TArray<uint8>& PcmData, float& OutRms, float& OutPeak, float& OutClipRatio) const;
	void StartSpeech(const FString& Text);
};
