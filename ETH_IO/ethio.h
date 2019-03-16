#pragma once
#include <cstring>
#include <string>
#include <WinSock2.h>
#include <thread>
#include "utils/rapidjson/document.h"
#include <command.h>
#include <stdio.h>
//#include "Train.h"

[event_receiver(native)]
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
		int Connect( void );
	    int StartReceive( void );
	    void *ActualConnectTrain;
	    void HookTrain(void *trainObj);
	    void UnHookTrain(void);
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

		//SendingFunctions
	    int NetWrite(const char *Data, size_t Size);
		int SendFrame(std::string FrameType, int Value);

		//Events Handlers

		void OnInteriorlightChangedEventHandler(int Action);
	    void OnReverserChangedEventHandler(int Action);
	    void OnInstrumentlightChangedEventHandler(int Action);
};