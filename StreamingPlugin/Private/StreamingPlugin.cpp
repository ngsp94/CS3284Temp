// Some copyright should be here...

#include "StreamingPluginPrivatePCH.h"

#include <stdio.h>
//#include <string>
//#include <iostream>
#include <sstream>
//#include <iomanip>
#include "HttpRequestAdapter.h"
#include "HttpModule.h"
#include "IHttpResponse.h"

#define LOCTEXT_NAMESPACE "CStreamingPluginModule"

#define PIXEL_SIZE 4
#define BASE_PORT_NUM 30000
#define FPS 30 // frames per second
#define CHECK_PLAYER_TIME 10 // in seconds

#define SERVER_URL "http://localhost:8000"
#define AUTH_URL "/api-token-auth/"
#define GAME_SESSION_URL "/game-session/"

// temporary robot username and password
#define USERNAME "abc"
#define PASSWORD "abc"

DEFINE_LOG_CATEGORY(ModuleLog)

void CStreamingPluginModule::StartupModule()
{
	// timer to capture frames
	FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &CStreamingPluginModule::CaptureFrame), 1.0 / FPS);

	// timer to check player join/quit
	FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &CStreamingPluginModule::CheckPlayers), CHECK_PLAYER_TIME);

	// init class variables
	AuthToken = "";
	GetToken();
}


void CStreamingPluginModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.


}


// call this for each player join
void CStreamingPluginModule::SetUpPlayer(int ControllerId) {

	// encode and write players' frames to http stream
	std::stringstream *StringStream = new std::stringstream();
	// Need to replace the http ip with actual address when running Unreal Engine
	*StringStream << "ffmpeg -y " << " -f rawvideo -pix_fmt rgba -s " << halfSizeX << "x" << halfSizeY << " -r " << FPS << " -i - -listen 1 -c:v libx264 -preset slow -f avi -an -tune zerolatency http://192.168.0.70:" << BASE_PORT_NUM + ControllerId << " 2> out" << ControllerId << ".txt";
	UE_LOG(ModuleLog, Warning, TEXT("stream started"));
	VideoPipeList.Add(_popen(StringStream->str().c_str(), "wb"));

	// add frame buffer for new player
	TArray<FColor> TempFrameBuffer;
	FrameBufferList.Add(TempFrameBuffer);

	PlayerFrameMapping.Add(ControllerId);

}


void CStreamingPluginModule::SetUpVideoCapture() {

	// init frame dimension variables
	FViewport* ReadingViewport = GEngine->GameViewport->Viewport;
	sizeX = ReadingViewport->GetSizeXY().X;
	sizeY = ReadingViewport->GetSizeXY().Y;
	halfSizeX = sizeX / 2;
	halfSizeY = sizeY / 2;
	UE_LOG(ModuleLog, Warning, TEXT("Height: %d Width: %d"), sizeY, sizeX);

	// set up split screen info
	Screen1 = FIntRect(0, 0, halfSizeX, halfSizeY);
	Screen2 = FIntRect(halfSizeX, 0, sizeX, halfSizeY);
	Screen3 = FIntRect(0, halfSizeY, halfSizeX, sizeY);
	Screen4 = FIntRect(halfSizeX, halfSizeY, sizeX, sizeY);
	flags = FReadSurfaceDataFlags(ERangeCompressionMode::RCM_MinMaxNorm, ECubeFace::CubeFace_NegX);

}


bool CStreamingPluginModule::CaptureFrame(float DeltaTime) {
	//UE_LOG(ModuleLog, Warning, TEXT("time %f"), DeltaTime); // can track running time


	// engine has been started
	if (!isEngineRunning && GEngine->GameViewport != nullptr && GIsRunning && IsInGameThread()) {
		UE_LOG(ModuleLog, Warning, TEXT("engine started"));
		isEngineRunning = true;
		SetUpVideoCapture();
		NumberOfPlayers = 1;
		SetUpPlayer(0);


	}

	// engine has been stopped
	else if (isEngineRunning && !(GEngine->GameViewport != nullptr && GIsRunning && IsInGameThread())) {
		isEngineRunning = false;
		UE_LOG(ModuleLog, Warning, TEXT("engine stopped"));

		for (int i = 0; i < NumberOfPlayers; i++) {
			// flush and close video pipes
			int PipeIndex = PlayerFrameMapping[i];
			fflush(VideoPipeList[PipeIndex]);
			fclose(VideoPipeList[PipeIndex]);
		}
	}

	if (GEngine->GameViewport != nullptr && GIsRunning && IsInGameThread())
	{
		// split screen for 4 players
		Split4Player();
		StreamFrameToClient();
	}
	return true;
}


void CStreamingPluginModule::StreamFrameToClient() {

	// use VideoPipe (class variable) to pass frames to encoder
	uint32 *PixelBuffer;
	FColor Pixel;
	PixelBuffer = new uint32[sizeX * sizeY * PIXEL_SIZE];

	for (int i = 0; i < NumberOfPlayers; i++) {
		int FrameSize = FrameBufferList[i].Num();

		for (int j = 0; j < FrameSize; ++j) {
			Pixel = FrameBufferList[PlayerFrameMapping[i]][j];
			PixelBuffer[j] = Pixel.A << 24 | Pixel.B << 16 | Pixel.G << 8 | Pixel.R;
		}

		fwrite(PixelBuffer, halfSizeX * PIXEL_SIZE, halfSizeY, VideoPipeList[i]);
	}
	delete[]PixelBuffer;
}


// Split screen for 4 player
void CStreamingPluginModule::Split4Player() {

	FViewport* ReadingViewport = GEngine->GameViewport->Viewport;

	if (NumberOfPlayers > 0)
		ReadingViewport->ReadPixels(FrameBufferList[0], flags, Screen1);
	if (NumberOfPlayers > 1)
		ReadingViewport->ReadPixels(FrameBufferList[1], flags, Screen2);
	if (NumberOfPlayers > 2)
		ReadingViewport->ReadPixels(FrameBufferList[2], flags, Screen3);
	if (NumberOfPlayers > 3)
		ReadingViewport->ReadPixels(FrameBufferList[3], flags, Screen4);

}

bool CStreamingPluginModule::GetToken()
{
	FString Url = SERVER_URL AUTH_URL;
	FString ContentString;

	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
	JsonObject->SetStringField(TEXT("username"), USERNAME);
	JsonObject->SetStringField(TEXT("password"), PASSWORD);

	TSharedRef<TJsonWriter<TCHAR>> JsonWriter = TJsonWriterFactory<>::Create(&ContentString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter);

	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetURL(Url);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetContentAsString(ContentString);
	HttpRequest->OnProcessRequestComplete().BindRaw(this, &CStreamingPluginModule::OnAuthResponseComplete);

	return HttpRequest->ProcessRequest(); // returns boolean: successful or not

}

void CStreamingPluginModule::OnAuthResponseComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (bWasSuccessful)
	{
		UE_LOG(ModuleLog, Warning, TEXT("Response Code = %d"), Response->GetResponseCode());

		if ((Response.IsValid()) && EHttpResponseCodes::IsOk(Response->GetResponseCode()))
		{
			TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
			TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(Response->GetContentAsString());
			FJsonSerializer::Deserialize(JsonReader, JsonObject);

			AuthToken = JsonObject->GetStringField("token");

			UE_LOG(ModuleLog, Warning, TEXT("Token = %s"), *AuthToken);

		}
		else
		{
			UE_LOG(ModuleLog, Warning, TEXT("Request failed! Response invalid"));
		}
	}
	else
	{
		UE_LOG(ModuleLog, Warning, TEXT("Request failed! Is the server up?"));
	}

}

// check with server if any players joined/quit
bool CStreamingPluginModule::CheckPlayers(float DeltaTime)
{

	if (GEngine->GameViewport != nullptr && GIsRunning && IsInGameThread())
	{
		UE_LOG(ModuleLog, Warning, TEXT("Checking players"));

		// get controller info from server
		FString Url = SERVER_URL GAME_SESSION_URL;

		TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
		AuthToken = "Token " + AuthToken;
		HttpRequest->SetHeader(TEXT("Authorization"), AuthToken);
		HttpRequest->SetURL(Url);
		HttpRequest->SetVerb(TEXT("GET"));
		HttpRequest->OnProcessRequestComplete().BindRaw(this, &CStreamingPluginModule::OnGetResponseComplete);
		bool RequestSuccess = HttpRequest->ProcessRequest();

		UE_LOG(ModuleLog, Warning, TEXT("URL = %s"), *Url);
	}

	return true;
}


void CStreamingPluginModule::OnGetResponseComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{

	if (bWasSuccessful)
	{

		UE_LOG(ModuleLog, Warning, TEXT("Response Code = %d"), Response->GetResponseCode());

		if (!Response.IsValid())
		{
			UE_LOG(ModuleLog, Warning, TEXT("Request failed!"));
		}
		else if (EHttpResponseCodes::IsOk(Response->GetResponseCode()))
		{
			TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());

			// split json array elements
			FString JsonString = Response->GetContentAsString();
			TArray<FString> JsonElementList;
			JsonString = JsonString.Replace(TEXT("["), TEXT(""));
			JsonString = JsonString.Replace(TEXT("]"), TEXT(""));
			JsonString.ParseIntoArray(JsonElementList, TEXT("},"), 1);

			// update controller and streaming port info
			TArray<int> NewActivePlayers;
			if (NumberOfPlayers != JsonElementList.Num())
			{
				NumberOfPlayers = JsonElementList.Num();
				for (int i = 0; i < JsonElementList.Num(); i++)
				{
					if (i < JsonElementList.Num() - 1)
					{
						JsonElementList[i] = JsonElementList[i] + "}";
					}

					//UE_LOG(ModuleLog, Warning, TEXT("element = %s"), *JsonElementList[i]);

					TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(JsonElementList[i]);
					FJsonSerializer::Deserialize(JsonReader, JsonObject);
					int32 ControllerId = JsonObject->GetIntegerField("controller");
					int32 StreamingPort = JsonObject->GetIntegerField("streaming_port");

					//UE_LOG(ModuleLog, Warning, TEXT("controller id = %d, streaming port = %d"), ControllerId, StreamingPort);

					// check for player join
					if (!PlayerFrameMapping.Contains(ControllerId)) // new player
					{
						UE_LOG(ModuleLog, Warning, TEXT("Player %d joined"), ControllerId);
						SetUpPlayer(ControllerId);
					}
					NewActivePlayers.Add(ControllerId);
				}
				// check for any player quit
				for (int i = 0; i < PlayerFrameMapping.Num(); i++)
				{
					int32 ControllerId = PlayerFrameMapping[i];
					if (!NewActivePlayers.Contains(ControllerId)) // player quit
					{
						UE_LOG(ModuleLog, Warning, TEXT("Player %d quit"), ControllerId);
						int PipeIndex = PlayerFrameMapping[ControllerId];
						fflush(VideoPipeList[PipeIndex]);
						fclose(VideoPipeList[PipeIndex]);
						PlayerFrameMapping.Remove(ControllerId);
						VideoPipeList.RemoveAt(PipeIndex);
					}
				}
			}
		}
		else
		{
			UE_LOG(ModuleLog, Warning, TEXT("Request failed! Response invalid"));
		}
	}
	else
	{
		UE_LOG(ModuleLog, Warning, TEXT("Request failed! Is the server up?"));
	}

}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(CStreamingPluginModule, StreamingPlugin)