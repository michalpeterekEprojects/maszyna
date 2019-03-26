#include "Camera.h"
#include "widgets/trainingcard.h"
#include <exception>
#include "utils/rapidjson/document.h"
#include "utils/rapidjson/writer.h"
#include "utils/rapidjson/stringbuffer.h"
#include "Globals.h"
#include "Logs.h"

CamRecorder::CamRecorder()
{
	this->Connect();
}

CamRecorder::~CamRecorder()
{

}

int CamRecorder::StartRecording( void )
{
	return 0;
}

int CamRecorder::EndRecording( std::string training_identifier ) {

	return 0;
}

void CamRecorder::ReceiveDataTask(CamRecorder *Object)
{
	char recvbuf[8192] = "";

	int iResult = SOCKET_ERROR;
	while (isReceive)
	{
		char *precbvuf = recvbuf;
		int bytesRecv = 0;
		bool FrameComplete = false;
		memset(recvbuf, 0x00, sizeof(recvbuf));
		do
		{
			iResult = SOCKET_ERROR;
			iResult = recv(this->Socket, precbvuf++, 1, 0);
		} while (!FrameComplete);
		if (iResult == SOCKET_ERROR)
		{
			WriteLog("CamRecorder : Socket error!");
			int iReconnectResult = 0;
			do
			{
				WriteLog("CamRecorder : Try reconnect!");
				Sleep(2000);
				closesocket(this->Socket);
				shutdown(this->Socket, SD_BOTH);
				iReconnectResult = this->Connect();
			} while (iReconnectResult != 1);
			continue;
		}


		if (this->ProcessIncomingData(recvbuf, static_cast<size_t>(bytesRecv)))
		{
			// WriteLog("ETH : Parse OK!");
		}
		else
		{
			WriteLog("CamRecorder : Parse ERR!");
		}
	}
}


int CamRecorder::Connect()
{

	this->Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (this->Socket == INVALID_SOCKET)
	{
		WriteLog("CamRecorder : Error creating socket!");
		WSACleanup();
		return -1;
	}
	service.sin_family = AF_INET;
	service.sin_addr.s_addr = inet_addr(Global.CamRecorder_conf.CameraManagerIP.c_str());
	service.sin_port = htons(Global.CamRecorder_conf.CameraManagerPort);

	int Timeout = Global.ethio_conf.ReceiveTimeout; /* Timeout */

	setsockopt(this->Socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Timeout, sizeof(Timeout));

	if (connect(this->Socket, (SOCKADDR *)&service, sizeof(service)) == SOCKET_ERROR)
	{
		WriteLog("CamRecorder : Connection error.");
		WriteLog("CamRecorder : Connection details : Host - " + Global.ethio_conf.ControllerIP + " Port - " + to_string(Global.ethio_conf.ControllerPort));
		// WSACleanup();
		return -1;
	}
	else
	{
		WriteLog("CamRecorder : Connection established \r\n : Host - " + Global.ethio_conf.ControllerIP + " \r\n Port - " + to_string(Global.ethio_conf.ControllerPort));
		return 1;
	}

	return 0;
}


int CamRecorder::NetWrite(const char *Data, size_t Size)
{
	return send(this->Socket, Data, static_cast<int>(Size), 0);
}

int CamRecorder::SendStartRecoringCMD( void )
{
	int iResult = 0;

	do
	{
		rapidjson::Document JsonObject;
		JsonObject.SetObject();
		rapidjson::Document::AllocatorType &allocator = JsonObject.GetAllocator();

		rapidjson::Value Event(rapidjson::kStringType);

		Event.SetString("start", allocator);
		JsonObject.AddMember("CMD", Event, allocator);
	 
		/* Event.SetInt(Value);
		JsonObject.AddMember("Value", Event, allocator);*/

		// object.AddMember("Event", FrameType, allocator);
		// object.AddMember("Data", Value, allocator);

		rapidjson::StringBuffer strbuf;
		rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
		JsonObject.Accept(writer);

		strbuf.Put('\r');
		strbuf.Put('\n');

		if (!this->NetWrite(strbuf.GetString(), strbuf.GetLength()))
			break;

		iResult = 1;
	} while (0);

	return iResult;
}

int CamRecorder::SendStopRecoringCMD( std::string ftp_url, std::string ftp_user, std::string ftp_password )
{
	int iResult = 0;

	do
	{
		rapidjson::Document JsonObject;
		JsonObject.SetObject();
		rapidjson::Document::AllocatorType &allocator = JsonObject.GetAllocator();

		rapidjson::Value Event(rapidjson::kStringType);

		Event.SetString("stop", allocator);
		JsonObject.AddMember("CMD", Event, allocator);
		
		Event.SetString(ftp_url.c_str(), allocator);
		JsonObject.AddMember("ftp_url", Event, allocator);
		
		Event.SetString(ftp_user.c_str(), allocator);
		JsonObject.AddMember("ftp_user", Event, allocator);
		
		Event.SetString(ftp_password.c_str(), allocator);
		JsonObject.AddMember("ftp_password", Event, allocator);

		/* Event.SetInt(Value);
		JsonObject.AddMember("Value", Event, allocator);*/

		// object.AddMember("Event", FrameType, allocator);
		// object.AddMember("Data", Value, allocator);

		rapidjson::StringBuffer strbuf;
		rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
		JsonObject.Accept(writer);

		strbuf.Put('\r');
		strbuf.Put('\n');

		if (!this->NetWrite(strbuf.GetString(), strbuf.GetLength()))
			break;

		iResult = 1;
	} while (0);

	return iResult;
}