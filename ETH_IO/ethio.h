#pragma once
#include <cstring>
#include <string>
#include <WinSock2.h>
#include <thread>
#include "utils/rapidjson/document.h"
#include <command.h>
class ethio
{
	public:
		struct conf_t
		{
			bool enable = false;
			std::string ControllerIP;
			int ControllerPort;
		};

		ethio();
		~ethio();
		int Connect();
	    int StartReceive();
	private:
	    WSADATA wsaData;
	    SOCKET Socket;
	    sockaddr_in service;
	    void ReceiveDataTask(ethio *Object);
	    std::thread ReceiveThread;
	    bool isReceive = false;
	    int ProcessIncomingData(char *Data, size_t DataSize);
	    int ParseDataFrame(rapidjson::Document * Value);
	    int WriteCommand(std::string CMD);
	    int WriteCommand(std::string CMD, int Value);
	    int WriteCommand(std::string CMD, double Value);
	    const char *JSON_NAMES[2] = 
		{
			"CMD",
			"VALUE"
		};
		enum
		{
			CMD,
			VALUE
		}e_JSON_NAMES;
	    command_relay relay;
};