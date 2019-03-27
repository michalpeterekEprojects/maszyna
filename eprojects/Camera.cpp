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
	closesocket(this->Socket);
	shutdown(this->Socket, SD_BOTH);
}

int CamRecorder::StartRecording( void )
{
	int iResult = 0;
	do
	{
		iResult = this->SendStartRecoringCMD();
		if (iResult != 1)
			break;
		iResult = this->WaitForStartCMDResponse();
	} while (0);
		
	return iResult;
}

int CamRecorder::EndRecording( std::string training_identifier ) {
	int iResult = 0;

	for (int chno = 0; chno < training_identifier.length(); chno++)
	{
		if (training_identifier[chno] == ' ')
			training_identifier[chno] = '_';
	}

	char Ftp_Dir[128];
	memset(Ftp_Dir, 0x00, sizeof(Ftp_Dir));
	snprintf(Ftp_Dir, (sizeof(Ftp_Dir) - 1), "ftp://%s/%s/%s.avi", Global.CamRecorder_conf.FTP_ip.c_str(), Global.CamRecorder_conf.FTP_dir.c_str(), training_identifier.c_str());

	do
	{
		iResult = this->SendStopRecoringCMD(Ftp_Dir, Global.CamRecorder_conf.FTP_User, Global.CamRecorder_conf.FTP_Password);
		iResult = this->WaitForEndCMDResponse();
	} while (0);


	return iResult;
}

int CamRecorder::ReceiveData(rapidjson::Document * ReceivedData)
{
	char recvbuf[8192] = "";

	int iResult = SOCKET_ERROR;
	while (1)
	{
		char *precbvuf = recvbuf;
		int bytesRecv = 0;
		bool FrameComplete = false;
		memset(recvbuf, 0x00, sizeof(recvbuf));
		do
		{
			iResult = SOCKET_ERROR;
			iResult = recv(this->Socket, precbvuf++, 1, 0);

			if (*(precbvuf - 1) == '\n')
			{
				*(precbvuf - 1) = 0x00;
				*(precbvuf - 2) = 0x00;
				bytesRecv -= 2;
				WriteLog("CamRecorder : Data RECV : ");
				WriteLog(recvbuf);
				FrameComplete = true;
				break;
			}
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
		} while (!FrameComplete);


		if (this->ProcessIncomingData(recvbuf, static_cast<size_t>(bytesRecv), ReceivedData))
		{
			WriteLog("CamRecorder : Parse OK!");
			return 1;
		}
		else
		{
			WriteLog("CamRecorder : Parse ERR!");
			return 0;
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
		WriteLog("CamRecorder : Connection details : Host - " + Global.CamRecorder_conf.CameraManagerIP + " Port - " + to_string(Global.CamRecorder_conf.CameraManagerPort));
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

int CamRecorder::ProcessIncomingData(char *Data, size_t DataSize, rapidjson::Document * ResponseData)
{
	//ResponseData  = new rapidjson::Document();

	do
	{
		ResponseData->Parse(Data, DataSize);

		if (ResponseData->Capacity() == 0)
			break;
		else
		{
			return 1;
		}
	} while (0);
	//delete ResponseData;
	return 0; //
}

int CamRecorder::WaitForStartCMDResponse(void) {
	
	int iResult = 0;
	rapidjson::Document JsonObject;
	do
	{
		iResult = ReceiveData(&JsonObject);
		if (iResult != 1)
			break;
		if (JsonObject.HasMember("Status"))
		{
			if (JsonObject["Status"].GetBool())
			{
				iResult = 1;
			}
			else
			{
				WriteLog("CamRecorder : Failed to start recording!");
				iResult = 0;
			}

			if (JsonObject.HasMember("Message"))
				WriteLog(JsonObject["Message"].GetString());
		}
		else
		{
			WriteLog("CamRecorder : Bad frame!");
			iResult = 0;
			break;
		}
	} while (0);
	
	return iResult;
}

int CamRecorder::WaitForEndCMDResponse(void)
{
	int iResult = 0;
	int ResponseCount = 1;
	rapidjson::Document JsonObject;
	do
	{
		iResult = ReceiveData(&JsonObject);
		if (iResult != 1)
			break;
		if (JsonObject.HasMember("Status"))
		{
			if (JsonObject["Status"].GetBool())
			{
				iResult = 1;
			}
			else
			{
				WriteLog("CamRecorder : Failed to end recording!");
				iResult = 0;
				break;
			}

			if (JsonObject.HasMember("Message"))
				WriteLog(JsonObject["Message"].GetString());
		}
		else
		{
			WriteLog("CamRecorder : Bad frame!");
			iResult = 0;
			break;
		}
	} while (ResponseCount--);

	return iResult;
}