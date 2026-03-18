// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RuntimeAudioImporterTypes.h"
#include "LocalTTSComponent.generated.h"

class UAudioComponent;
class URuntimeAudioImporterLibrary;
class UImportedSoundWave;
class USoundWave;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTTSAudioReady, const FString&, Text, USoundWave*, SoundWave);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnTTSPcmReady, const FString&, Text, int32, SampleRate, const TArray<uint8>&, PcmData);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnTTSPcmFloatReady, const FString&, Text, int32, SampleRate, const TArray<float>&, PcmFloat);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnTTSSpeechResult, const FString&, Text, const TArray<uint8>&, AudioData, int32, SampleRate, int32, NumChannels, ERuntimeRAWAudioFormat, RawFormat);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnTTSPlaybackStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnTTSPlaybackFinished);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTTSError, const FString&, Error);

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class LIPSYNCTEST_API ULocalTTSComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULocalTTSComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	FString PiperExePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	FString ModelPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	FString ModelConfigPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	bool bUseCuda = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	int32 SpeakerId = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	float LengthScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	float NoiseScale = 0.667f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS")
	float NoiseW = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Playback")
	bool bAutoPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Playback")
	float Volume = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Playback")
	float Pitch = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Playback", meta = (ClampMin = "0.0"))
	float FadeMs = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Playback", meta = (ClampMin = "0.0"))
	float PadMs = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Playback")
	bool bQueueSpeech = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Streaming")
	bool bBufferedSpeech = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Streaming", meta = (ClampMin = "1"))
	int32 BufferMinWords = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Streaming")
	bool bFlushOnPunctuation = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Quality")
	bool bFallbackToWavOnCorrupt = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Quality")
	bool bForceWavOutput = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|LipSync")
	bool bBroadcastFloatPcm = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local TTS|Runtime Audio Importer")
	bool bUseRuntimeAudioImporter = false;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS")
	FOnTTSAudioReady OnTTSAudioReady;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS")
	FOnTTSPcmReady OnTTSPcmReady;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS")
	FOnTTSPcmFloatReady OnTTSPcmFloatReady;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS")
	FOnTTSSpeechResult OnTTSSpeechResult;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS", meta = (DisplayName = "OnSpeechResult"))
	FOnTTSSpeechResult OnSpeechResult;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS")
	FOnTTSPlaybackStarted OnTTSPlaybackStarted;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS")
	FOnTTSPlaybackFinished OnTTSPlaybackFinished;

	UPROPERTY(BlueprintAssignable, Category = "Local TTS")
	FOnTTSError OnTTSError;

	UFUNCTION(BlueprintCallable, Category = "Local TTS")
	void Speak(const FString& Text);

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Text To Speech"), Category = "Local TTS")
	void TextToSpeech(const FString& Text);


	UFUNCTION(BlueprintCallable, Category = "Local TTS|Streaming")
	void AppendSpeechChunk(const FString& Text);

	UFUNCTION(BlueprintCallable, Category = "Local TTS|Streaming")
	void FlushSpeechBuffer();

	UFUNCTION(BlueprintCallable, Category = "Local TTS")
	void Interrupt();

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	TObjectPtr<UAudioComponent> AudioComponent;

	UPROPERTY()
	TObjectPtr<USoundWave> LastSound;

	UPROPERTY(Transient)
	TObjectPtr<URuntimeAudioImporterLibrary> AudioImporter;

	FDelegateHandle ImporterResultHandle;

	TArray<uint8> LastPcmData;
	int32 LastPcmSampleRate = 0;

	int32 SpeakGeneration = 0;
	bool bIsGenerating = false;
	bool bIsPlaying = false;
	TArray<FString> PendingTexts;
	FTimerHandle PlaybackTimer;
	FString SpeechBuffer;

	FString ResolvePath(const FString& Path) const;
	void EmitError(const FString& Message) const;
	bool LoadSampleRate(int32& OutSampleRate, FString& OutError) const;
	bool RunPiperToRawPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const;
	bool RunPiperToWavPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const;
	bool ParseWavToPcm16(const TArray<uint8>& WavData, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const;
	void CreateAndPlaySound(const FString& Text, const TArray<uint8>& PcmData, int32 SampleRate);
	void StartSpeech(const FString& Text);
	void OnPlaybackFinished();
	void ClearQueue();
	int32 CountWords(const FString& Text) const;
};
