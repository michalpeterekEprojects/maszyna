#include "stdafx.h"
#include "ethio.h"
#include "Globals.h"
#include "simulation.h"
#include "Train.h"
#include "parser.h"
#include "Logs.h"
#include "simulation.h"
#include "Train.h"
#include <exception>
#include "utils/rapidjson/document.h"
#include "utils/rapidjson/writer.h"
#include "utils/rapidjson/stringbuffer.h"

ethio::ethio()
{
	int iResult = 0;

	iResult = WSAStartup(MAKEWORD(2, 2), &this->wsaData);

	memset(&service, 0, sizeof(service));
	if (iResult != NO_ERROR)
	{
		WriteLog("ETH : Init error!");
		this->~ethio();
	}
}

ethio::~ethio()
{
	this->isReceive = false;
	WSACleanup();
}

void ethio::HookTrain(void *trainObj)
{
	if (trainObj != nullptr)
	{
		__hook(&TTrain::OnInteriorlightChanged, reinterpret_cast<TTrain *>(trainObj), &ethio::OnInteriorlightChangedEventHandler);
		__hook(&TTrain::OnReverserChanged, reinterpret_cast<TTrain *>(trainObj), &ethio::OnReverserChangedEventHandler);
		__hook(&TTrain::OnInstrumentlightChanged, reinterpret_cast<TTrain *>(trainObj), &ethio::OnInstrumentlightChangedEventHandler);
		this->ActualConnectTrain = trainObj;
	}
}

void ethio::UnHookTrain( void )
{
	if (this->ActualConnectTrain != nullptr)
	{
		__unhook(&TTrain::OnInteriorlightChanged, reinterpret_cast<TTrain *>(this->ActualConnectTrain), &ethio::OnInteriorlightChangedEventHandler);
		__unhook(&TTrain::OnReverserChanged, reinterpret_cast<TTrain *>(this->ActualConnectTrain), &ethio::OnReverserChangedEventHandler);
		__unhook(&TTrain::OnInstrumentlightChanged, reinterpret_cast<TTrain *>(this->ActualConnectTrain), &ethio::OnInstrumentlightChangedEventHandler);
	}
}

int ethio::Connect()
{

	this->Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (this->Socket == INVALID_SOCKET)
	{
		WriteLog("ETH : Error creating socket!");
		WSACleanup();
		return -1;
	}
	service.sin_family = AF_INET;
	service.sin_addr.s_addr = inet_addr(Global.ethio_conf.ControllerIP.c_str());
	service.sin_port = htons(Global.ethio_conf.ControllerPort);

	int Timeout = Global.ethio_conf.ReceiveTimeout; /* Timeout */

	setsockopt(this->Socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Timeout, sizeof(Timeout));

	if (connect(this->Socket, (SOCKADDR *)&service, sizeof(service)) == SOCKET_ERROR)
	{
		WriteLog("ETH : Connection error.");
		WriteLog("ETH : Connection details : Host - " + Global.ethio_conf.ControllerIP + " Port - " + to_string(Global.ethio_conf.ControllerPort));
		//WSACleanup();
		return -1;
	}
	else
	{
		WriteLog("ETH : Connection established \r\n : Host - " + Global.ethio_conf.ControllerIP + " \r\n Port - " + to_string(Global.ethio_conf.ControllerPort));
		return 1;
	}

	return 0;
}

int ethio::SetSendTimeout(int ms)
{
	return setsockopt(this->Socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(int));
}

int ethio::StartReceive()
{
	this->isReceive = true;
	this->ReceiveThread = std::thread(&ethio::ReceiveDataTask, this, this);

	return 1;
}

void ethio::ReceiveDataTask(ethio *Object)
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
			if (iResult == SOCKET_ERROR)
			{
				switch (WSAGetLastError())
				{
					case WSAETIMEDOUT:
					{
					    
						WriteLog("ETH : Controller not responding!");
						iResult = SOCKET_ERROR;
					    goto Error;
					}
					break;

					default:
					    goto Error;
				}
			}
			else
			{
				if (this->PausedByConnErr)
				{
					Global.iPause = 0;
					this->PausedByConnErr = false;
				}
				bytesRecv += iResult;

				if (*(precbvuf - 1) == '\n')
				{
					*(precbvuf - 1) = 0x00;
					*(precbvuf - 2) = 0x00;
					bytesRecv -= 2;
					FrameComplete = true;
					break;
				}
			}

		} while (!FrameComplete);
		Error:
		if (iResult == SOCKET_ERROR)
		{
			WriteLog("ETH : Socket error!");
			int iReconnectResult = 0;
			do
			{
				Global.iPause = 1;
				this->PausedByConnErr = true;
				WriteLog("ETH : Try reconnect!");
				Sleep(2000);
				closesocket(this->Socket);
				shutdown(this->Socket, SD_BOTH);
				iReconnectResult = this->Connect();
			} while (iReconnectResult != 1);
			continue;
		}

		//std::string LogMessage = "ETH : Message : ";

		//std::string RecvData(recvbuf);

		//WriteLog(LogMessage + RecvData);

		if (this->ProcessIncomingData(recvbuf, static_cast<size_t>(bytesRecv)))
		{
			//WriteLog("ETH : Parse OK!");
		}
		else
		{
			WriteLog("ETH : Parse ERR!");
		}
	}
}

int ethio::ProcessIncomingData(char *Data, size_t DataSize)
{
	rapidjson::Document JsonObject;

	do
	{
		JsonObject.Parse(Data, DataSize);

		if (JsonObject.Capacity() == 0)
			break;
		else
		{
			return this->ParseDataFrame(&JsonObject);
		}
	} while (0);

	return 0; // 
}

int ethio::ParseDataFrame(rapidjson::Document *Value)
{

	int iResult = 0;

	do
	{

		if (Value->HasMember("CMD") && Value->HasMember("Value") && Value->HasMember("Type"))
		{
			if ((*Value)["Value"].IsInt())
				iResult = this->WriteCommand((*Value)["CMD"].GetString(), (*Value)["Value"].GetInt(), (*Value)["Type"].GetInt());
			else
			{
				WriteLog("ETH : Unknown arg Type!");
				return 0;
			}
		}
		else if(Value->HasMember("CMD") && Value->HasMember("Value"))
		{
			if ((*Value)["Value"].IsInt())
				iResult = this->WriteCommand((*Value)["CMD"].GetString(), (*Value)["Value"].GetInt());
			else if ((*Value)["Value"].IsDouble())
				iResult = this->WriteCommand((*Value)["CMD"].GetString(), (*Value)["Value"].GetDouble());
			else
			{
				WriteLog("ETH : Unknown arg Type!");
				return 0;
			}
		}
		else if (Value->HasMember("CMD"))
		{
			iResult = this->WriteCommand((*Value)["CMD"].GetString());
		}
		else if (Value->HasMember("STATUS"))
		{
			if (simulation::is_ready != this->simulationReady)
			{
				simulationReady = simulation::is_ready;
				this->SendFrame("SimulationReady", simulationReady ? 1 : 0);
			}
			if (Global.iPause != this->simulationState)
			{
				this->simulationState = Global.iPause;
				this->SendFrame("SimulationState", this->simulationState);
			}
			return 1;
		}

	} while (0);

	return iResult;
}

int ethio::WriteCommand(std::string CMD)
{
	

	if (CMD == "hornhighactivate")
	{
		relay.post(user_command::hornhighactivate, 0, 0, GLFW_PRESS,0);
	}
	else if(CMD == "hornhighdeactivate")
	{
		relay.post(user_command::hornhighactivate, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "hornlowactivate")
	{
		relay.post(user_command::hornlowactivate, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "hornlowdeactivate")
	{
		relay.post(user_command::hornlowactivate, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "alerteracknowledge_push")
	{
		relay.post(user_command::alerteracknowledge, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "alerteracknowledge_release")
	{
		relay.post(user_command::alerteracknowledge, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "whistleactivate")
	{
		relay.post(user_command::whistleactivate, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "whistledeactivate")
	{
		relay.post(user_command::whistleactivate, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "radiotoggle_push")
	{
		relay.post(user_command::radiotoggle, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "radiotoggle_release")
	{
		relay.post(user_command::radiotoggle, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "radiostoptest_push")
	{
		relay.post(user_command::radiostoptest, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "radiostoptest_release")
	{
		relay.post(user_command::radiostoptest, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "radiostopsend_push")
	{
		relay.post(user_command::radiostopsend, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "radiostopsend_release")
	{
		relay.post(user_command::radiostopsend, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "radiochanneldecrease_push")
	{
		relay.post(user_command::radiochanneldecrease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "radiochanneldecrease_release")
	{
		relay.post(user_command::radiochanneldecrease, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "radiochannelincrease_push")
	{
		relay.post(user_command::radiochannelincrease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "radiochannelincrease_release")
	{
		relay.post(user_command::radiochannelincrease, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "alarmchaintoggle_push")
	{
		relay.post(user_command::alarmchaintoggle, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "alarmchaintoggle_release")
	{
		relay.post(user_command::alarmchaintoggle, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "linebreakertoggle_push")
	{
		relay.post(user_command::linebreakertoggle, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "linebreakertoggle_release")
	{
		relay.post(user_command::linebreakertoggle, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "trainbrakefirstservice")
	{
		relay.post(user_command::trainbrakefirstservice, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "trainbrakefullservice")
	{
		relay.post(user_command::trainbrakefullservice, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "trainbrakeservice")
	{
		relay.post(user_command::trainbrakeservice, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "trainbrakeemergency")
	{
		relay.post(user_command::trainbrakeemergency, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "trainbrakerelease")
	{
		relay.post(user_command::trainbrakerelease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "independentbrakebailoff_push")
	{
		relay.post(user_command::independentbrakebailoff, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "independentbrakebailoff_release")
	{
		relay.post(user_command::independentbrakebailoff, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "manualbrakeincrease_push")
	{
		relay.post(user_command::manualbrakeincrease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "manualbrakeincrease_release")
	{
		relay.post(user_command::manualbrakeincrease, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "interiorlightenable")
	{
		relay.post(user_command::interiorlightenable, 0, 0, GLFW_PRESS, 0);
		relay.post(user_command::interiorlightenable, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "interiorlightdisable")
	{
		relay.post(user_command::interiorlightdimdisable, 0, 0, GLFW_PRESS, 0);
		relay.post(user_command::interiorlightdimdisable, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "instrumentlightenable")
	{
		relay.post(user_command::instrumentlightenable, 0, 0, GLFW_PRESS, 0);
		relay.post(user_command::instrumentlightenable, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "instrumentlightdisable")
	{
		relay.post(user_command::instrumentlightdisable, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "headlightsdimenable")
	{
		relay.post(user_command::headlightsdimenable, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "headlightsdimdisable")
	{
		relay.post(user_command::headlightsdimdisable, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "wheelspinbrakeactivate_push")
	{
		relay.post(user_command::wheelspinbrakeactivate, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "wheelspinbrakeactivate_release")
	{
		relay.post(user_command::wheelspinbrakeactivate, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "reverserforward")
	{
		relay.post(user_command::reverserforward, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "reverserneutral")
	{
		relay.post(user_command::reverserneutral, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "reverserbackward")
	{
		relay.post(user_command::reverserbackward, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "pantographtogglefront_push")
	{
		relay.post(user_command::pantographtogglefront, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "pantographtogglefront_release")
	{
		relay.post(user_command::pantographtogglefront, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "independentbrakeincrease_push")
	{
		relay.post(user_command::independentbrakeincrease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "independentbrakeincrease_release")
	{
		relay.post(user_command::independentbrakeincrease, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "independentbrakeincreasefast_push")
	{
		relay.post(user_command::independentbrakeincreasefast, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "independentbrakeincreasefast_release")
	{
		relay.post(user_command::independentbrakeincreasefast, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "independentbrakedecrease_push")
	{
		relay.post(user_command::independentbrakedecrease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "independentbrakedecrease_release")
	{
		relay.post(user_command::independentbrakedecrease, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "independentbrakedecreasefast_push")
	{
		relay.post(user_command::independentbrakedecreasefast, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "independentbrakedecreasefast_release")
	{
		relay.post(user_command::independentbrakedecreasefast, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "cabsignalacknowledge_push")
	{
		relay.post(user_command::cabsignalacknowledge, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "cabsignalacknowledge_release")
	{
		relay.post(user_command::cabsignalacknowledge, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "secondcontrollerincrease_push")
	{
		relay.post(user_command::secondcontrollerincrease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "secondcontrollerincrease_release")
	{
		relay.post(user_command::secondcontrollerincrease, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "secondcontrollerdecrease_push")
	{
		relay.post(user_command::secondcontrollerdecrease, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "secondcontrollerdecrease_release")
	{
		relay.post(user_command::secondcontrollerdecrease, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "batteryenable")
	{
		relay.post(user_command::batteryenable, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "batterydisable")
	{
		relay.post(user_command::batterydisable, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "linebreakeropen_push")
	{
		relay.post(user_command::linebreakeropen, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "linebreakeropen_release")
	{
		relay.post(user_command::linebreakeropen, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "linebreakerclose_push")
	{
		relay.post(user_command::linebreakerclose, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "linebreakerclose_release")
	{
		relay.post(user_command::linebreakerclose, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "pantographraisefront_push")
	{
		relay.post(user_command::pantographraisefront, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "pantographraisefront_release")
	{
		relay.post(user_command::pantographraisefront, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "pantographraiserear_push")
	{
		relay.post(user_command::pantographraiserear, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "pantographraiserear_release")
	{
		relay.post(user_command::pantographraiserear, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "pantographlowerfront_push")
	{
		relay.post(user_command::pantographlowerfront, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "pantographlowerfront_release")
	{
		relay.post(user_command::pantographlowerfront, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "pantographlowerrear_push")
	{
		relay.post(user_command::pantographlowerrear, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "pantographlowerrear_release")
	{
		relay.post(user_command::pantographlowerrear, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "sandboxactivate_push")
	{
		relay.post(user_command::sandboxactivate, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "sandboxactivate_release")
	{
		relay.post(user_command::sandboxactivate, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "batterytoggle_push")
	{
		relay.post(user_command::batterytoggle, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "batterytoggle_release")
	{
		relay.post(user_command::batterytoggle, 0, 0, GLFW_RELEASE, 0);
	}
	else if (CMD == "alarmchainenable")
	{
		relay.post(user_command::alarmchainenable, 0, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "alarmchaindisable")
	{
		relay.post(user_command::alarmchaindisable, 0, 0, GLFW_PRESS, 0);
	}
	else
	{
		WriteLog("ETH : Unknown command - " + CMD);
		return 0;
	}
	WriteLog("ETH : Command recv - " + CMD);
	return 1;
}

int ethio::WriteCommand(std::string CMD, int Value, int Type)
{
	WriteLog("ETH : Command recv - " + CMD + "Value : " + std::to_string(Value));
	if (CMD == "radiobutton") // RadioButtons
	{
		switch (Value)
		{
			case 0:
			{
			    
			}
		    break;
		    case 1:
		    {

		    }
		    break;
		    case 2:
		    {

		    }
		    break;
		    case 3:
		    {

		    }
		    break;
		    case 4:
		    {
			    if ( Type == 0)
					relay.post(user_command::radiostoptest, 0, 0, GLFW_PRESS, 0);
			    else	
					relay.post(user_command::radiostoptest, 0, 0, GLFW_RELEASE, 0);
		    }
		    break;
		    case 5:
		    {

		    }
		    break;
		    case 6:
		    {

		    }
		    break;
		    case 7:
		    {

		    }
		    break;
		    case 8:
		    {

		    }
		    break;
		    case 9:
		    {

		    }
		    break;
		    case 10:
		    {

		    }
		    break;
		    case 11:
		    {

		    }
		    break;
		    case 12:
		    {

		    }
		    break;
		    case 13:
		    {

		    }
		    break;
		    case 14:
		    {

		    }
		    break;
		}
	}
	return 1;
}

int ethio::WriteCommand(std::string CMD, double Value)
{
	WriteLog("ETH : Command recv - " + CMD + "Value : " + std::to_string(Value));
	if (CMD == "mastercontrollerset") 
	{
		switch (static_cast<int>(Value))
		{
			case -3:
			{
			    relay.post(user_command::mastercontrollerset, 0, 0, GLFW_PRESS, 0);
			    return 1;
			}
		    break;
		    case -2:
		    {
			    relay.post(user_command::mastercontrollerset, 1, 0, GLFW_PRESS, 0);
			    return 1;
		    }
		    break;
		    case -1:
		    {
			    relay.post(user_command::mastercontrollerset, 2, 0, GLFW_PRESS, 0);
			    return 1;
		    }
		    break;
		    case 0:
		    {
			    relay.post(user_command::mastercontrollerset, 3, 0, GLFW_PRESS, 0);
			    return 1;
		    }
		    break;
		    case 1:
		    {
			    relay.post(user_command::mastercontrollerset, 4, 0, GLFW_PRESS, 0);
			    return 1;
		    }
		    break;
		    case 2:
		    {
			    relay.post(user_command::mastercontrollerset, 5, 0, GLFW_PRESS, 0);
			    return 1;
		    }
		    break;
		    case 3:
		    {
			    relay.post(user_command::mastercontrollerset, 6, 0, GLFW_PRESS, 0);
			    return 1;
		    }
		    break;
		    case 4:
		    {
			    relay.post(user_command::mastercontrollerset, 7, 0, GLFW_PRESS, 0);
			    return 1;
		    }
		    break;
		    default:
			    WriteLog("ETH : Bad mastercontroller Value!");
			break;
		}
		
	}
	else if (CMD == "independentbrakeset")
	{
		relay.post(user_command::independentbrakeset, Value, 0, GLFW_PRESS, 0);
	}
	else if (CMD == "trainbrakeset") // This is emulate phisical controller ! Must be implemented somewhere else!
	{
		switch (static_cast<int>(Value))
		{
			case -1:
			{
			    relay.post(user_command::trainbrakeset, 0.0, 0, GLFW_PRESS, 0);
			}
			break;
			case 0:
			{
			    relay.post(user_command::trainbrakeset, 0.2, 0, GLFW_PRESS, 0);
			}
			break;
			case 1:
			{
			    relay.post(user_command::trainbrakeset, 0.4, 0, GLFW_PRESS, 0);
			}
			break;
			case 2:
			{
			    relay.post(user_command::trainbrakeset, 0.6, 0, GLFW_PRESS, 0);
			}
			break;
			case 3:
			{
			    relay.post(user_command::trainbrakeset, 0.8, 0, GLFW_PRESS, 0);
			}
			break;
			case 4:
			{
			    relay.post(user_command::trainbrakeset, 1.0, 0, GLFW_PRESS, 0);
			}
		    break;
		    default:
			    WriteLog("ETH : Bad trainbrake Value!");
			    break;
		}
	}
	else if (CMD == "secondcontrollerset") // This is emulate phisical controller ! Must be implemented somewhere else!
	{
		switch (static_cast<int>(Value))
		{
			case 0:
			{
			    relay.post(user_command::secondcontrollerset, 0.0, 0, GLFW_PRESS, 0);
			}
		    break;
		    case 1:
		    {
			    relay.post(user_command::secondcontrollerset, 1.0, 0, GLFW_PRESS, 0);
		    }
		    break;
		    case 2:
		    {
			    relay.post(user_command::secondcontrollerset, 2.0, 0, GLFW_PRESS, 0);
		    }
		    break;
		    case 3:
		    {
			    relay.post(user_command::secondcontrollerset, 3.0, 0, GLFW_PRESS, 0);
		    }
		    break;
		    case 4:
		    {
			    relay.post(user_command::secondcontrollerset, 4.0, 0, GLFW_PRESS, 0);
		    }
		    break;
		}
	}
	return 1;
}

int ethio::NetWrite(const char *Data, size_t Size)
{
	return send(this->Socket, Data, static_cast<int>(Size), 0);
}

int ethio::SendFrame(std::string FrameType, int Value)
{
	int iResult = 0;

	do
	{
		rapidjson::Document JsonObject;
		JsonObject.SetObject();
		rapidjson::Document::AllocatorType &allocator = JsonObject.GetAllocator();

		rapidjson::Value Event(rapidjson::kStringType);

		Event.SetString(FrameType.c_str(), allocator);
		JsonObject.AddMember("Event", Event, allocator);

		Event.SetInt(Value);
		JsonObject.AddMember("Value", Event, allocator);

		//object.AddMember("Event", FrameType, allocator);
		//object.AddMember("Data", Value, allocator);

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

void ethio::OnInteriorlightChangedEventHandler(int Action)
{
	

	if (this->SendFrame("InteriorlightChanged", Action))
	{
		WriteLog("ETH : Event send OK!");
	}
	else
	{
		WriteLog("ETH : Event send ERR!");
	}
}

void ethio::OnReverserChangedEventHandler(int Action)
{
	if (this->SendFrame("ReverserChanged", Action))
	{
		WriteLog("ETH : Event send!");
	}
	else
	{
		WriteLog("ETH : Event send ERR!");
	}
}


void ethio::OnInstrumentlightChangedEventHandler(int Action)
{
	if (this->SendFrame("InstrumentlightChanged", Action))
	{
		WriteLog("ETH : Event send!");
	}
	else
	{
		WriteLog("ETH : Event send ERR!");
	}
}