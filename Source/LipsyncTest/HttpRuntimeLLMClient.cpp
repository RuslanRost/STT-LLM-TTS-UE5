#include "HttpRuntimeLLMClient.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UHttpRuntimeLLMClient* UHttpRuntimeLLMClient::CreateRuntimeLLMClient()
{
	UHttpRuntimeLLMClient* Client = NewObject<UHttpRuntimeLLMClient>();
	Client->ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	return Client;
}

void UHttpRuntimeLLMClient::SendPing()
{
	SendJsonRequest(TEXT("GET"), TEXT("/ping"), FString());
}

void UHttpRuntimeLLMClient::StartLLMServer()
{
	SendJsonRequest(TEXT("POST"), TEXT("/start_llm"), TEXT("{}"));
}

void UHttpRuntimeLLMClient::StopLLMServer()
{
	SendJsonRequest(TEXT("POST"), TEXT("/stop_llm"), TEXT("{}"));
}

void UHttpRuntimeLLMClient::GetLLMStatus()
{
	SendJsonRequest(TEXT("GET"), TEXT("/status"), FString());
}

void UHttpRuntimeLLMClient::SendPrompt(const FString& Prompt, const FString& SystemPrompt, float Temperature)
{
	if (ClientId.IsEmpty())
	{
		ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	}

	const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("prompt"), Prompt);
	Payload->SetStringField(TEXT("system_prompt"), SystemPrompt);
	Payload->SetNumberField(TEXT("temperature"), Temperature);
	Payload->SetStringField(TEXT("client_id"), ClientId);
	Payload->SetStringField(TEXT("request_id"), RequestId);

	FString JsonBody;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	FJsonSerializer::Serialize(Payload, Writer);

	const FString Url = BuildUrl(TEXT("/prompt"));
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
	Request->SetContentAsString(JsonBody);
	Request->OnProcessRequestComplete().BindUObject(this, &UHttpRuntimeLLMClient::HandlePromptResponse, Prompt, Url, RequestId);
	Request->ProcessRequest();
}

FString UHttpRuntimeLLMClient::BuildUrl(const FString& Route) const
{
	const FString TrimmedBase = BaseUrl.EndsWith(TEXT("/")) ? BaseUrl.LeftChop(1) : BaseUrl;
	if (Route.IsEmpty())
	{
		return TrimmedBase;
	}

	return Route.StartsWith(TEXT("/")) ? TrimmedBase + Route : TrimmedBase + TEXT("/") + Route;
}

void UHttpRuntimeLLMClient::SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody)
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

	Request->OnProcessRequestComplete().BindUObject(this, &UHttpRuntimeLLMClient::HandleResponse, UpperMethod, Url);
	Request->ProcessRequest();
}

void UHttpRuntimeLLMClient::HandleResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Method, FString Url)
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

	OnRequestFailed.Broadcast(Method, Url, FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody));
}

void UHttpRuntimeLLMClient::HandlePromptResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Prompt, FString Url, FString RequestId)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		OnRequestFailed.Broadcast(TEXT("POST"), Url, TEXT("Prompt request failed or server unavailable."));
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (!EHttpResponseCodes::IsOk(StatusCode))
	{
		OnRequestFailed.Broadcast(TEXT("POST"), Url, FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody));
		return;
	}

	FString Reply;
	FString ResponseRequestId = RequestId;
	FString ResponseClientId = ClientId;
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("reply"), Reply);
		Root->TryGetStringField(TEXT("request_id"), ResponseRequestId);
		Root->TryGetStringField(TEXT("client_id"), ResponseClientId);
	}

	OnRequestSucceeded.Broadcast(TEXT("POST"), Url, StatusCode, ResponseBody);
	if (!Reply.IsEmpty())
	{
		OnPromptResponse.Broadcast(Prompt, Reply);
		OnPromptResponseDetailed.Broadcast(Prompt, Reply, ResponseRequestId, ResponseClientId);
	}
}
