#include "CloudyWebAPIPrivatePCH.h"
 
#include "CloudyWebAPI.h"

#include "PlatformFeatures.h"
#include "HttpRequestAdapter.h"
#include "HttpModule.h"
#include "IHttpResponse.h"
#include "AllowWindowsPlatformTypes.h"
#include "ThirdParty/libcurl/include/Windows/curl/curl.h"
#include "HideWindowsPlatformTypes.h"
#include "string"
#include "../../CloudyPanelPlugin/Public/CloudyPanelPlugin.h"


DEFINE_LOG_CATEGORY(CloudyWebAPILog);

#define SERVER_NAME "Listener"
#define SERVER_ENDPOINT FIPv4Endpoint(FIPv4Address(127, 0, 0, 1), 55556)
#define CONNECTION_THREAD_TIME 5 // in seconds
#define BUFFER_SIZE 1024
#define MAX_PLAYERS 4

#define SUCCESS_MSG "Success"
#define FAILURE_MSG "Failure"

static FString BaseUrl;
static const FString AuthUrl = "/api-token-auth/";
static const FString SaveDataUrl = "/save-data/";
static FString Token;           // Robot's authentication token
static FString SaveFileURL0;    // URL for player controller 0
static FString SaveFileURL1;    // URL for player controller 1
static FString SaveFileURL2;    // URL for player controller 2
static FString SaveFileURL3;    // URL for player controller 3
FString HttpResponse;

// Foward declaration
std::string get_env_var(std::string const & key);

// Automatically starts when UE4 is started.
// Populates the Token variable with the robot user's token.
void CloudyWebAPIImpl::StartupModule()
{
    UE_LOG(CloudyWebAPILog, Warning, TEXT("CloudyWebAPI started"));

    // BaseUrl will be updated with the correct URL
    BaseUrl = get_env_var("CLOUDYWEB_URL").c_str();
    // Token variable will be populated with the robot user's token.
    AttemptAuthentication();

	// Set up socket listener to receive commands from CloudyWeb

	//Create Socket
	FIPv4Endpoint Endpoint(SERVER_ENDPOINT);
	ListenSocket = FTcpSocketBuilder(SERVER_NAME).AsReusable().BoundToEndpoint(Endpoint).Listening(8);

	//Set Buffer Size
	int32 NewSize = 0;
	ListenSocket->SetReceiveBufferSize(BUFFER_SIZE, NewSize);

	TcpListener = new FTcpListener(*ListenSocket, CONNECTION_THREAD_TIME);
	TcpListener->OnConnectionAccepted().BindRaw(this, &CloudyWebAPIImpl::InputHandler);

	FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &CloudyWebAPIImpl::CheckConnection), CONNECTION_THREAD_TIME);


	// initialise class variables
	InputStr = "";
	HasInputStrChanged = false;

}

// Automatically starts when UE4 is closed
void CloudyWebAPIImpl::ShutdownModule()
{
    UE_LOG(CloudyWebAPILog, Warning, TEXT("CloudyWebAPI stopped"));
	delete TcpListener;
	ListenSocket->Close();
}

/**
* This function is called when the libcurl function "CURLOPT_WRITEFUNCTION" 
* demands it. It reads the response data from the server, which can be put
* into a buffer using the libcurl function "CURLOPT_WRITEDATA".
*
*/
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}


/**
* Writes data into file. Callback function for the libcurl function 
* "CURLOPT_WRITEFUNCTION". Used for writing into files.
*
*/
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

/**
* Gets the value of a system environment variable
*
* @param key     Variable name of the environment variable that you want.
*
* @return Returns the value corresponding to the given parameter key.
*
*/
std::string get_env_var(std::string const & key) {
    char * val;
    val = getenv(key.c_str());
    std::string retval = "";
    if (val != NULL) {
        retval = val;
    }
    return retval;
}

/**
* Uses the robot user's username and password to obtain an operator token to use for
* various functions.  The username and password is obtained from the system environment variables.
*
*
* @return Returns true if the authentication was successful. Else, returns false.
*/
bool CloudyWebAPIImpl::AttemptAuthentication()
{
    bool RequestSuccess = false;

    FString Username;
    FString Password;
    FString value = get_env_var("ROBOT_USER").c_str();
    value.Split(";", &Username, &Password);
    UE_LOG(CloudyWebAPILog, Warning, TEXT("RobotUserName = %s, RobotPassword = %s"), 
           *Username.Trim(), *Password.Trim());

    FString Url = BaseUrl + AuthUrl; // "http://127.0.0.1:8000/api-token-auth/";
    FString ContentString;

    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
    JsonObject->SetStringField(TEXT("username"), Username.Trim());
    JsonObject->SetStringField(TEXT("password"), Password.Trim());

    TSharedRef<TJsonWriter<TCHAR>> JsonWriter = TJsonWriterFactory<>::Create(&ContentString);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter);

    TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    HttpRequest->SetURL(Url);
    HttpRequest->SetVerb(TEXT("POST"));
    HttpRequest->SetContentAsString(ContentString);
    HttpRequest->OnProcessRequestComplete().BindRaw(this, &CloudyWebAPIImpl::OnAuthResponseComplete);
    RequestSuccess = HttpRequest->ProcessRequest();

    UE_LOG(CloudyWebAPILog, Warning, TEXT("URL = %s"), *Url);

    return RequestSuccess;
}

/**
* Uploads the save file to CloudyWeb.
*
* @param Filename               Filename of the save game file.
* @param PlayerControllerId     PlayerControllerId of the player who is saving the game.
*
* @return Returns true if the file upload was successful. Else, returns false.
*
*/
bool CloudyWebAPIImpl::UploadFile(FString Filename, int32 PlayerControllerId)
{
    bool RequestSuccess = false;

    CURL *curl;
    CURLcode res;
    std::string readBuffer;
    
    FString Url = BaseUrl + SaveDataUrl;
    std::string UrlCString(TCHAR_TO_UTF8(*Url));
    
    // Filepath of .sav file
    FString Filepath = FPaths::GameDir();
    Filepath += "Saved/SaveGames/" + Filename + ".sav";
    std::string filePath(TCHAR_TO_UTF8(*Filepath));
    
    // Get game name
    FString GameName = FApp::GetGameName();
    std::string gameName(TCHAR_TO_UTF8(*GameName));
    
    // Convert PlayerControllerId
    FString playerControllerIdFString = FString::FromInt(PlayerControllerId);
    std::string playerControllerId(TCHAR_TO_UTF8(*playerControllerIdFString));
    
    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;
    struct curl_slist *headerlist = NULL;
    FString AuthHeader = "Authorization: Token " + Token;
    std::string AuthHeaderCString(TCHAR_TO_UTF8(*AuthHeader));
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    /* Fill in the file upload field */
    curl_formadd(&formpost, &lastptr,
        CURLFORM_COPYNAME, "saved_file",
        CURLFORM_FILE, filePath.c_str(),
        CURLFORM_END);
    
    /* Fill in the player controller ID field */
    curl_formadd(&formpost, &lastptr,
        CURLFORM_COPYNAME, "controller",
        CURLFORM_COPYCONTENTS, playerControllerId.c_str(),
        CURLFORM_END);
    
    /* Fill in the game name field */
    curl_formadd(&formpost, &lastptr,
        CURLFORM_COPYNAME, "game",
        CURLFORM_COPYCONTENTS, gameName.c_str(),
        CURLFORM_END);
    
    curl = curl_easy_init();
    /* initialize custom header list (stating that Expect: 100-continue is not
    wanted */
    headerlist = curl_slist_append(headerlist, AuthHeaderCString.c_str());
    if (curl) {
        /* what URL that receives this POST */
        curl_easy_setopt(curl, CURLOPT_URL, UrlCString.c_str());

        /* only disable 100-continue header if explicitly requested */
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);

        /* What form to send */
        curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
    
        /* Set up string to write response into */
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    
        /* Perform the request, res will get the return code */
        res = curl_easy_perform(curl);

        /* Check if the request was successful */
        if (res == CURLE_OK)
        {
            RequestSuccess = true;
        }
    
        /* always cleanup */
        curl_easy_cleanup(curl);
    
        /* then cleanup the formpost chain */
        curl_formfree(formpost);
        /* free slist */
        curl_slist_free_all(headerlist);
    
        UE_LOG(CloudyWebAPILog, Warning, TEXT("Response data: %s"), UTF8_TO_TCHAR(readBuffer.c_str()));
        ReadAndStoreSaveFileURL(readBuffer.c_str(), PlayerControllerId);
    }

    return RequestSuccess;
}


/**
* Downloads the save file from CloudyWeb.
*
* @param Filename               Filename of the save game file.
* @param PlayerControllerId     Player controller ID of the player who requested the file.
*
* @return Returns true if the file download was successful. Else, returns false.
*
*/
bool CloudyWebAPIImpl::DownloadFile(FString Filename, int32 PlayerControllerId)
{
    CURL *curl;
    FILE *fp;
    CURLcode res;
    errno_t err;
    std::string SaveFileURLCString;

    if (PlayerControllerId == 0)
    {
        SaveFileURLCString = TCHAR_TO_UTF8(*SaveFileURL0);
    }
    else if (PlayerControllerId == 1)
    {
        SaveFileURLCString = TCHAR_TO_UTF8(*SaveFileURL1);
    }
    else if (PlayerControllerId == 2)
    {
        SaveFileURLCString = TCHAR_TO_UTF8(*SaveFileURL2);
    }
    else if (PlayerControllerId == 3)
    {
        SaveFileURLCString = TCHAR_TO_UTF8(*SaveFileURL3);
    }

    // Filepath of .sav file
    FString Filepath = FPaths::GameDir();
    Filepath += "Saved/SaveGames/" + Filename + "-" + 
                FString::FromInt(PlayerControllerId) + ".sav";
    std::string filePath(TCHAR_TO_UTF8(*Filepath));

    curl = curl_easy_init();
    if (curl) {
        if ((err = fopen_s(&fp, filePath.c_str(), "wb")) == 0)
        {
            curl_easy_setopt(curl, CURLOPT_URL, SaveFileURLCString.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            fclose(fp);
        }
        else
        {
            UE_LOG(CloudyWebAPILog, Error, TEXT("Could not create save file!"));
        }
    }
    return true;
}

/**
* This function parses the Json response after uploading the save file to obtain the 
* URL of the save file.
*
* @param JsonString              Json string to parse
* @param PlayerControllerId      Player controller ID of the player who is saving the file
*
*/
void CloudyWebAPIImpl::ReadAndStoreSaveFileURL(FString JsonString, int32 PlayerControllerId)
{
    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
    TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(JsonString);
    FJsonSerializer::Deserialize(JsonReader, JsonObject);

    if (PlayerControllerId == 0)
    {
        SaveFileURL0 = JsonObject->GetStringField("saved_file");
    }
    else if (PlayerControllerId == 1)
    {
        SaveFileURL1 = JsonObject->GetStringField("saved_file");
    }
    else if (PlayerControllerId == 2)
    {
        SaveFileURL2 = JsonObject->GetStringField("saved_file");
    }
    else if (PlayerControllerId == 3)
    {
        SaveFileURL3 = JsonObject->GetStringField("saved_file");
    }
}

/**
* This is a callback function that is called when the "AttemptAuthentication" function
* has completed its request.
*
* @param Request            
* @param Response           Contains the data of the response, including the response code.
* @param bWasSuccessful     Contains true if the request was successful. Else it contains false.
*
*/
void CloudyWebAPIImpl::OnAuthResponseComplete(FHttpRequestPtr Request, 
                                              FHttpResponsePtr Response, bool bWasSuccessful)
{
    if (bWasSuccessful)
    {
        UE_LOG(CloudyWebAPILog, Warning, TEXT("Response Code = %d"), Response->GetResponseCode());

        if (!Response.IsValid())
        {
            UE_LOG(CloudyWebAPILog, Warning, TEXT("Request failed!"));
        }
        else if (EHttpResponseCodes::IsOk(Response->GetResponseCode()))
        {
            TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
            TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(Response->GetContentAsString());
            FJsonSerializer::Deserialize(JsonReader, JsonObject);

            Token = JsonObject->GetStringField("token");
            UE_LOG(CloudyWebAPILog, Warning, TEXT("Token = %s"), *Token);
        }
    }
    else 
    {
        UE_LOG(CloudyWebAPILog, Error, TEXT("Request failed! Is the server up?"));
    }
}


/**
* Makes a request to CloudyWeb server
*
* @param ResourceUrl The url of the resource to get, for example /game-session/
* @param RequestMethod The type of HTTP request, for example GET or DELETE
* @param Response The response from the server
*
* @return Whether the request was successful or not
*
*/
bool CloudyWebAPIImpl::MakeRequest(FString ResourceUrl, FString RequestMethod)
{
    UE_LOG(CloudyWebAPILog, Warning, TEXT("Getting resource: %s"), *ResourceUrl);
    FString Url = BaseUrl + ResourceUrl;
    HttpResponse = "";
    
    // use token to get resource from CloudyWeb
    FString AuthHeader = "Token " + Token;
    TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetHeader(TEXT("Authorization"), AuthHeader);
    HttpRequest->SetURL(Url);
    HttpRequest->SetVerb(RequestMethod);
    HttpRequest->OnProcessRequestComplete().BindRaw(this, &CloudyWebAPIImpl::OnGetResponseComplete);
    
    return HttpRequest->ProcessRequest();

}

/**
* Accessor for HTTP response. Caller may have to wait for valid response.
*
* @return The response from the HTTP request from MakeRequest
*
*/
FString CloudyWebAPIImpl::GetResponse()
{
    return HttpResponse;
}

/**
* Callback function for MakeRequest. Sets the global response variable.
*
* @param Request
* @param Response           Contains the data of the response, including the response code.
* @param bWasSuccessful     Contains true if the request was successful. Else it contains false.
*
*/
void CloudyWebAPIImpl::OnGetResponseComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{

    if (bWasSuccessful)
    {
        UE_LOG(CloudyWebAPILog, Warning, TEXT("Response Code = %d"), Response->GetResponseCode());
    
        if (Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode()))
        {
            HttpResponse = Response->GetContentAsString();
        }
        else
        {
            UE_LOG(CloudyWebAPILog, Warning, TEXT("Request failed! Response invalid"));
        }
    }
    else
    {
        UE_LOG(CloudyWebAPILog, Warning, TEXT("Request failed! Is the server up?"));
    }

}

/**
* Parses string and stores as global variables for access by other modules
*
* @param InputStr Input string to parse
* @return Whether parsing was successful or not
*/
bool CloudyWebAPIImpl::GetCloudyWebData(FString InputStr)
{
	bool isSuccessful = false;
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
	TSharedRef<TJsonReader<TCHAR>>JsonReader = TJsonReaderFactory<TCHAR>::Create(InputStr);
	isSuccessful = FJsonSerializer::Deserialize(JsonReader, JsonObject);

	// these 2 fields will be populated for both join/quit commands
	Command = JsonObject->GetStringField("command");
	ControllerId = JsonObject->GetIntegerField("controller");

	// somehow, there is no try function for integer field
	if (JsonObject->HasField("streaming_port")) 
	{
		StreamingPort = JsonObject->GetIntegerField("streaming_port");
	}
	
	if (JsonObject->HasField("game_id"))
	{
		GameId = JsonObject->GetIntegerField("game_id");
	}
	if (JsonObject->HasField("game_session_id"))
	{
		GameSessionId = JsonObject->GetIntegerField("game_session_id");
	}

	JsonObject->TryGetStringField("streaming_ip", StreamingIP);
	FString Username;
	JsonObject->TryGetStringField("username", Username);
	UsernameList.Insert(Username, ControllerId);
	
	return isSuccessful;
}


/**
* Timer to periodically check for join/quit signals from client, and call
* appropriate input handler.
*
* @param DeltaTime Time taken by method
*/
bool CloudyWebAPIImpl::CheckConnection(float DeltaTime)
{
	bool Success = false;
	if (HasInputStrChanged) 
	{
		if (GEngine->GameViewport != nullptr && GIsRunning && IsInGameThread())
		{
			UE_LOG(CloudyWebAPILog, Warning, TEXT("Success! input str: %s"), *InputStr);
			GetCloudyWebData(InputStr);
			UE_LOG(CloudyWebAPILog, Warning, TEXT("Success! Controllerid: %d command: %s"), ControllerId, *Command);
			Success = CCloudyPanelPluginModule::Get().ExecuteCommand(Command, ControllerId, StreamingPort, StreamingIP, GameSessionId);
			InputStr = "";
			HasInputStrChanged = false;
		}		

		// Send response to client
		if (Success)
		{
			SendToClient(TCPConnection, SUCCESS_MSG);
		}
		else
		{
			SendToClient(TCPConnection, FAILURE_MSG);
		}

	}

	return true; // continue timer to check for requests
}

/**
* Helper method to send message to client
*
* @param Socket The TCP socket used to send the message
* @param Msg The message to be sent
*/
bool CloudyWebAPIImpl::SendToClient(FSocket* Socket, FString Msg)
{
	TCHAR *serialisedChar = Msg.GetCharArray().GetData();
	int32 size = FCString::Strlen(serialisedChar);
	int32 sent = 0;
	return Socket->Send((uint8*)TCHAR_TO_UTF8(serialisedChar), size, sent);

	return true;
}

/**
* Handles input passed by TCP listener
*
* @param ConnectionSocket The TCP socket connecting the listener and client
* @param Endpoint The endpoint of the socket connection
*/
bool CloudyWebAPIImpl::InputHandler(FSocket* ConnectionSocket, const FIPv4Endpoint& Endpoint)
{

	TArray<uint8> ReceivedData;
	uint32 Size;

	// wait for data to arrive
	while (!(ConnectionSocket->HasPendingData(Size)));

	// handle data - change global InputStr
	ReceivedData.Init(FMath::Min(Size, 65507u));

	int32 Read = 0;
	ConnectionSocket->Recv(ReceivedData.GetData(), ReceivedData.Num(), Read);
	FString ReceivedString = StringFromBinaryArray(ReceivedData);

	InputStr = ReceivedString;
	HasInputStrChanged = true;

	TCPConnection = ConnectionSocket;

	return true;

}

// Accessor for GameId. Returns -1 if GameId has not been sent
int32 CloudyWebAPIImpl::GetGameId()
{
	if (GameId > 0)
		return GameId;
	else
		return -1;
}

// Accessor for username by controller ID. Returns empty string if
// this controller id does not belong to any user
FString CloudyWebAPIImpl::GetUsername(int32 ControllerId)
{
	if (UsernameList.IsValidIndex(ControllerId))
	{
		return UsernameList[ControllerId];
	}
	else
	{
		return "";
	}
	
	
}

//Rama's String From Binary Array
//This function requires #include <string>
FString CloudyWebAPIImpl::StringFromBinaryArray(const TArray<uint8>& BinaryArray)
{
	//Create a string from a byte array!
	std::string cstr(reinterpret_cast<const char*>(BinaryArray.GetData()), BinaryArray.Num());
	return FString(cstr.c_str());
}
 
IMPLEMENT_MODULE(CloudyWebAPIImpl, CloudyWebAPI)