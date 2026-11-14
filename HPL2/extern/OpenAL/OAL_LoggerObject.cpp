
#include "OpenAL/OAL_LoggerObject.h"
#include "OpenAL/OAL_Helper.h"

#include "system/Platform.h"
#include "system/String.h"

#include <stdlib.h>
//#include <tchar.h>
#include <stdio.h>
#include <stdarg.h>

#if defined(_WIN32)
#define UNICODE
#include <shlobj.h>
#endif

static std::wstring BuildLogFilename ( const std::string& asFilename );


bool					iOAL_LoggerObject::mbLogEnabled = false;
eOAL_LogOutput			iOAL_LoggerObject::mLogOutput = eOAL_LogOutput_File;
eOAL_LogVerbose			iOAL_LoggerObject::mLogVerboseLevel = eOAL_LogVerbose_Low;
std::wstring			iOAL_LoggerObject::msLogFile = BuildLogFilename("OAL.log");

//---------------------------------------------------------------------------------------

void iOAL_LoggerObject::SetLogFilename ( const std::string& asLogFilename )
{
	msLogFile = BuildLogFilename ( asLogFilename );
}

//---------------------------------------------------------------------------------------

void iOAL_LoggerObject::LogMsg(const std::string& asIDStr, eOAL_LogVerbose aVerbose, eOAL_LogMsg aType, const char* asMessage, ...)
{
	if(!mbLogEnabled)
		return;
	if(mLogVerboseLevel < aVerbose)
		return;

	if(asMessage==NULL)
		return;
	
	std::string sMessage;

	char text[2048];
	va_list ap;
	va_start(ap, asMessage);
	vsnprintf(text, sizeof(text), asMessage, ap);
	va_end(ap);

	switch(aType)
	{
	case eOAL_LogMsg_Command:
		sMessage.append("[COMMAND] ");
		break;
	case eOAL_LogMsg_Info:
		sMessage.append("[INFO] ");
		break;
	case eOAL_LogMsg_Error:
		sMessage.append("[ERROR] ");
	}

	sMessage.append(asIDStr.c_str()).append(text);

	Write(sMessage);

}

//---------------------------------------------------------------------------------------

void iOAL_LoggerObject::Write( const std::string& asMessage )
{
	if (!mbLogEnabled)
		return;

	FILE* fLog;

	switch(mLogOutput)
	{
	case eOAL_LogOutput_File:
		fLog = hpl::cPlatform::OpenFile(msLogFile, L"a");
		if (fLog != NULL)
		{
			fwrite(asMessage.c_str(), sizeof(char), asMessage.size(), fLog);
			fclose(fLog);
		}
		break;
	case eOAL_LogOutput_Console:
		printf("%s",asMessage.c_str());
		break;
	default:
		break;
	}
}

//---------------------------------------------------------------------------------------

std::wstring BuildLogFilename ( const std::string& asFilename )
{
	std::wstring wsName;
	std::wstring wsTemp;
	wsName = hpl::cString::To16Char(asFilename);
	
	#if defined(_WIN32)

		WCHAR sPath[MAX_PATH];
		if(SUCCEEDED(SHGetFolderPath(NULL, 
			CSIDL_PERSONAL | CSIDL_FLAG_CREATE, 
			NULL,0,sPath))) 
		{
			wsTemp = std::wstring(sPath).append(L"/").append(wsName);
		}
		else
		{
			return L"";
		}
	#else
		std::string home = std::string(getenv("HOME"));
		wsTemp = hpl::cString::To16Char(home);
		wsTemp.append(L"/").append(wsName);
	#endif

	FILE* pTempFile = NULL;
	wchar_t buffer[100];

	for (unsigned int i = 1; i != 0; i++)
	{
		swprintf(buffer, 100, L"_%d.log", i);

		hpl::cPlatform::OpenFile(wsTemp.append(buffer), L"r");
		if (pTempFile)
		{
			fclose(pTempFile);
			//hpl::cPlatform::RemoveFile(iOAL_LoggerObject::GetLogFilename());
		}
		else
		{
			wsTemp.append(std::wstring(buffer));
			break;
		}
	}
	return wsTemp;
}

//---------------------------------------------------------------------------------------
