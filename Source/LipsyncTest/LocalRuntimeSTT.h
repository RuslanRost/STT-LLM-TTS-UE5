// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "LocalRuntimeSTT.generated.h"

class UWorld;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSTTPartial, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSTTFinal, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnKeywordDetected, const FString&, Keyword);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnWakeWordDetected, const FString&, WakeWord);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSTTError, const FString&, Error);

UCLASS(BlueprintType)
class LIPSYNCTEST_API ULocalRuntimeSTT : public UObject
{
	GENERATED_BODY()

public:
	ULocalRuntimeSTT();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	FString VoskDllPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	FString ModelPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	int32 SampleRate = 16000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	int32 NumChannels = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	bool bUseGrammar = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT", meta=(EditCondition="bUseGrammar"))
	TArray<FString> GrammarPhrases;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Keywords")
	TArray<FString> Keywords;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bUseWakeWord = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	TArray<FString> WakeKeywords;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bWakeOnPartial = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word", meta = (ClampMin = "0.1"))
	float ActiveListenSeconds = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bExtendActiveOnFinal = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bExtendActiveOnPartial = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bDelayActiveTimerUntilTTSFinished = true;

	// If true, after wake word is detected we only emit a single final result for the words after it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bWakeWordFocusMode = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Keywords")
	bool bDetectKeywordsInPartial = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Keywords", meta = (ClampMin = "0.0"))
	float KeywordCooldownSeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	bool bEnableWords = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogAudio = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogJson = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogWakeWord = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogKeywords = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging", meta = (ClampMin = "1"))
	int32 AudioLogEveryN = 50;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnSTTPartial OnPartial;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnSTTFinal OnFinal;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnKeywordDetected OnKeywordDetected;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnWakeWordDetected OnWakeWordDetected;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnSTTError OnError;

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Create Runtime STT", WorldContext = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "Runtime STT")
	static ULocalRuntimeSTT* CreateRuntimeSTT(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	bool StartListening();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	void StopListening();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	bool IsListening() const;

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void EnterStandby();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void EnterActive();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void NotifyTTSStarted();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void NotifyTTSEnded();

protected:
	virtual void BeginDestroy() override;

private:
	void EmitError(const FString& Message) const;
	void HandleResultJson(const FString& JsonText, bool bFinal);
	void CheckKeywords(const FString& Text);
	bool CheckWakeWords(const FString& Text, bool bFinal);
	void TransitionToActive(const FString& WakeWord);
	void HandleActiveTimeout();
	FString BuildGrammarJson() const;
	FString ResolvePath(const FString& Path) const;
	UWorld* GetWorldFromContext() const;
	FString ExtractPostWakeText(const FString& Text) const;
	void RequestFinalFromWorker();

	bool LoadVosk();
	void UnloadVosk();
	bool CreateRecognizer();
	void DestroyRecognizer();

private:
	void* VoskLibHandle = nullptr;
	void* VoskModel = nullptr;
	void* VoskRecognizer = nullptr;

	bool bListening = false;
	int32 CaptureNumChannels = 1;
	TMap<FString, double> LastKeywordTime;
	bool bActiveListening = true;
	bool bTTSActive = false;
	FTimerHandle ActiveTimer;
	bool bWaitingPostWakeFinal = false;
	FString LastWakeWord;
	FString PostWakeBuffer;
	FThreadSafeBool bRequestFinal = false;
	bool bHadSpeechSinceActive = false;

	TWeakObjectPtr<UObject> WorldContextObject;

	// Function pointers for Vosk C API
	typedef void* (*FnVoskModelNew)(const char* model_path);
	typedef void (*FnVoskModelFree)(void* model);
	typedef void* (*FnVoskRecognizerNew)(void* model, float sample_rate);
	typedef void* (*FnVoskRecognizerNewGrm)(void* model, float sample_rate, const char* grammar);
	typedef void (*FnVoskRecognizerFree)(void* recognizer);
	typedef int (*FnVoskRecognizerAcceptWaveform)(void* recognizer, const char* data, int length);
	typedef const char* (*FnVoskRecognizerResult)(void* recognizer);
	typedef const char* (*FnVoskRecognizerPartialResult)(void* recognizer);
	typedef const char* (*FnVoskRecognizerFinalResult)(void* recognizer);
	typedef void (*FnVoskRecognizerSetWords)(void* recognizer, int words);

	FnVoskModelNew VoskModelNew = nullptr;
	FnVoskModelFree VoskModelFree = nullptr;
	FnVoskRecognizerNew VoskRecognizerNew = nullptr;
	FnVoskRecognizerNewGrm VoskRecognizerNewGrm = nullptr;
	FnVoskRecognizerFree VoskRecognizerFree = nullptr;
	FnVoskRecognizerAcceptWaveform VoskRecognizerAcceptWaveform = nullptr;
	FnVoskRecognizerResult VoskRecognizerResult = nullptr;
	FnVoskRecognizerPartialResult VoskRecognizerPartialResult = nullptr;
	FnVoskRecognizerFinalResult VoskRecognizerFinalResult = nullptr;
	FnVoskRecognizerSetWords VoskRecognizerSetWords = nullptr;

	// Audio capture
	class FSTTWorker;
	FSTTWorker* Worker = nullptr;
};
