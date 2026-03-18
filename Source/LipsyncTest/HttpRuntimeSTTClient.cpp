#include "HttpRuntimeSTTClient.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UHttpRuntimeSTTClient* UHttpRuntimeSTTClient::CreateRuntimeSTTClient()
{
	UHttpRuntimeSTTClient* Client = NewObject<UHttpRuntimeSTTClient>();
	Client->ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	return Client;
}

void UHttpRuntimeSTTClient::SendPing()
{
	SendJsonRequest(TEXT("GET"), TEXT("/ping"), FString());
}

void UHttpRuntimeSTTClient::GetSTTStatus()
{
	SendJsonRequest(TEXT("GET"), TEXT("/status"), FString());
}

void UHttpRuntimeSTTClient::TranscribePCM16(const TArray<uint8>& AudioData, int32 SampleRate, int32 NumChannels, bool bEnableWords, const TArray<FString>& GrammarPhrases)
{
	if (AudioData.Num() <= 0)
	{
		OnError.Broadcast(TEXT("AudioData is empty."));
		return;
	}

	if (ClientId.IsEmpty())
	{
		ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	}

	const FString AudioBase64 = FBase64::Encode(AudioData);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("audio_base64"), AudioBase64);
	Payload->SetNumberField(TEXT("sample_rate"), SampleRate);
	Payload->SetNumberField(TEXT("num_channels"), NumChannels);
	Payload->SetBoolField(TEXT("enable_words"), bEnableWords);
	Payload->SetStringField(TEXT("client_id"), ClientId);
	Payload->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));

	TArray<TSharedPtr<FJsonValue>> GrammarValues;
	for (const FString& Phrase : GrammarPhrases)
	{
		GrammarValues.Add(MakeShared<FJsonValueString>(Phrase));
	}
	Payload->SetArrayField(TEXT("grammar_phrases"), GrammarValues);

	FString JsonBody;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	FJsonSerializer::Serialize(Payload, Writer);

	const FString Url = BuildUrl(TEXT("/transcribe_pcm"));
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
	Request->SetContentAsString(JsonBody);
	Request->OnProcessRequestComplete().BindUObject(this, &UHttpRuntimeSTTClient::HandleTranscribeResponse, Url);
	Request->ProcessRequest();
}

void UHttpRuntimeSTTClient::TranscribeFloatAudio(const TArray<float>& AudioData, int32 SampleRate, int32 NumChannels, bool bEnableWords, const TArray<FString>& GrammarPhrases)
{
	TArray<uint8> PCM16;
	PCM16.Reserve(AudioData.Num() * sizeof(int16));
	for (float Sample : AudioData)
	{
		const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
		const int16 PCM = static_cast<int16>(Clamped * 32767.0f);
		PCM16.Add(static_cast<uint8>(PCM & 0xFF));
		PCM16.Add(static_cast<uint8>((PCM >> 8) & 0xFF));
	}
	TranscribePCM16(PCM16, SampleRate, NumChannels, bEnableWords, GrammarPhrases);
}

FString UHttpRuntimeSTTClient::BuildUrl(const FString& Route) const
{
	const FString TrimmedBase = BaseUrl.EndsWith(TEXT("/")) ? BaseUrl.LeftChop(1) : BaseUrl;
	if (Route.IsEmpty())
	{
		return TrimmedBase;
	}
	return Route.StartsWith(TEXT("/")) ? TrimmedBase + Route : TrimmedBase + TEXT("/") + Route;
}

void UHttpRuntimeSTTClient::SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody)
{
	const FString UpperMethod = Method.ToUpper();
	const FString Url = BuildUrl(Route);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(UpperMethod);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
	if (!JsonBody.IsEmpty() && UpperMethod != TEXT("GET"))
	{
		Request->SetContentAsString(JsonBody);
	}
	Request->OnProcessRequestComplete().BindUObject(this, &UHttpRuntimeSTTClient::HandleResponse, UpperMethod, Url);
	Request->ProcessRequest();
}

void UHttpRuntimeSTTClient::HandleResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Method, FString Url)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		OnRequestFailed.Broadcast(Method, Url, TEXT("Request failed or server unavailable."));
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (EHttpResponseCodes::IsOk(StatusCode))
	{
		OnRequestSucceeded.Broadcast(Method, Url, StatusCode, ResponseBody);
		return;
	}

	const FString ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody);
	OnRequestFailed.Broadcast(Method, Url, ErrorMessage);
}

void UHttpRuntimeSTTClient::HandleTranscribeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Url)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		const FString ErrorMessage = TEXT("STT request failed or server unavailable.");
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		OnError.Broadcast(ErrorMessage);
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (!EHttpResponseCodes::IsOk(StatusCode))
	{
		const FString ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody);
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		OnError.Broadcast(ErrorMessage);
		return;
	}

	FString PartialText;
	FString FinalText;
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("partial"), PartialText);
		Root->TryGetStringField(TEXT("text"), FinalText);
	}

	OnRequestSucceeded.Broadcast(TEXT("POST"), Url, StatusCode, ResponseBody);
	if (!PartialText.IsEmpty())
	{
		OnPartial.Broadcast(PartialText);
	}
	OnFinal.Broadcast(FinalText);
}
