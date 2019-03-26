#pragma once

#include <cstring>
#include <string>
#include <WinSock2.h>
#include <thread>
#include "utils/rapidjson/document.h"

class CamRecorder
{
  public:
	struct conf_t
	{
		bool enable = false;
		std::string CameraManagerIP;
		int CameraManagerPort;
		std::string FTP_ip;
		std::string FTP_dir;
		std::string FTP_User;
		std::string FTP_Password;
	};
	CamRecorder();
	~CamRecorder();
	int StartRecording(void);
	int EndRecording(std::string training_identifier);

  private:
	WSADATA wsaData;
	SOCKET Socket;
	sockaddr_in service;
	void ReceiveDataTask(CamRecorder *Object);
	std::thread ReceiveThread;
	bool isReceive = false;
	int Connect(void);
	int ProcessIncomingData(char *Data, size_t DataSize);
	int ParseDataFrame(rapidjson::Document *Value);
	int NetWrite(const char *Data, size_t Size);
	int SendStartRecoringCMD( void );
	int SendStopRecoringCMD(std::string ftp_url, std::string ftp_user, std::string ftp_password);
};