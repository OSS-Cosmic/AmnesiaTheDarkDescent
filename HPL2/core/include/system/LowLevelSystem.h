/*
 * Copyright © 2009-2020 Frictional Games
 * 
 * This file is part of Amnesia: The Dark Descent.
 * 
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_LOWLEVELSYSTEM_H
#define HPL_LOWLEVELSYSTEM_H

#include "system/MemoryManager.h"
#include "system/SystemTypes.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#if defined(__clang__) || defined(__GNUC__)
#define NORETURN __attribute((__noreturn__))
#else
#define NORETURN
#endif

namespace hpl {

	//--------------------------------------------------------
#define UPDATE_TIMING_ENABLED
#ifdef UPDATE_TIMING_ENABLED
	#define START_TIMING_EX(x,y)	LogUpdate("Updating %s in file %s at line %d\n",x,__FILE__,__LINE__); \
								unsigned int y##_lTime = cPlatform::GetApplicationTime();
	#define START_TIMING(x)	LogUpdate("Updating %s in file %s at line %d\n",#x,__FILE__,__LINE__); \
								unsigned int x##_lTime = cPlatform::GetApplicationTime();
	#define STOP_TIMING(x)	LogUpdate(" Time spent: %d ms\n",cPlatform::GetApplicationTime() - x##_lTime);
	#define START_TIMING_TAB(x)	LogUpdate("\tUpdating %s in file %s at line %d\n",#x,__FILE__,__LINE__); \
							unsigned int x##_lTime = cPlatform::GetApplicationTime();
	#define STOP_TIMING_TAB(x)	LogUpdate("\t Time spent: %d ms\n",cPlatform::GetApplicationTime() - x##_lTime);
#else
	#define START_TIMING_EX(x,y)
	#define START_TIMING(x)
	#define STOP_TIMING(x)
	#define START_TIMING_TAB(x)
	#define STOP_TIMING_TAB(x)
#endif
	
	//--------------------------------------------------------

	class iScript;

	//--------------------------------------------------------

	extern void SetLogFile(const tWString &asFile);
	extern void FatalError(const char* fmt,... ) NORETURN;
	extern void Error(const char* fmt, ...);
	extern void Warning(const char* fmt, ...);
	extern void Log(const char* fmt, ...);

	//--------------------------------------------------------

	// A validation failure the caller cannot carry on from: the offending call
	// cannot be completed, so continuing leaves the caller acting on state that
	// was never established. In the renderer that means recording a draw or
	// dispatch against whatever the previous pass left bound, which corrupts the
	// GPU silently and surfaces later as an unrelated complaint -- exactly how
	// an invalid NRD image view presented, as "nothing bound root parameter 0"
	// pages away from the descriptor that actually caused it.
	//
	// Debug builds log and then assert, so the debugger stops at the failing
	// call with the stack intact; release builds terminate through FatalError.
	// Fatal either way, only the mechanism differs. The message is formatted
	// first because assert cannot carry one.
	//
	// Deliberately NOT NORETURN: the debug arm does return if the assert is
	// stepped over or compiled against a no-op handler, so call sites must still
	// yield a value afterwards. That also keeps the code that follows reachable
	// as far as the compiler is concerned, avoiding unreachable-code warnings.
	static inline void ValidationFailed(const char* fmt, ...)
	{
		char message[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(message, sizeof(message), fmt, args);
		va_end(args);
#if !defined(NDEBUG)
		Error("%s", message);
		assert(false && "validation failure");
#else
		FatalError("%s", message);
#endif
	}

	extern void SetUpdateLogFile(const tWString &asFile);
	extern void ClearUpdateLogFile();
	extern void SetUpdateLogActive(bool abX);
	extern bool GetUpdateLogActive();
	extern void LogUpdate(const char* fmt, ...);

	//--------------------------------------------------------

	enum eLogOutputType
	{
		eLogOutputType_Normal,
		eLogOutputType_Warning,
		eLogOutputType_Error,
		eLogOutputType_FatalError,
		eLogOutputType_Update,
		eLogOutputType_LastEnum,
	};
	
	typedef void(*tLogMessageCallbackFunc)(eLogOutputType aType, const char* asMessage); 

	extern void SetLogMessageCallback(tLogMessageCallbackFunc apFunc);

	//--------------------------------------------------------

	class iLowLevelSystem
	{
	public:
		virtual ~iLowLevelSystem(){}

		/**
		 * Creates a ne script
		 * \param asName name of the script.
		 * \return 
		 */
		virtual iScript* CreateScript(const tString& asName)=0;

		/**
		 * Add a function to the script vm. Example: "void test(float x)". must be __stdcall
		 * \param asFuncDecl the declaration.
		 * \return 
		 */
		virtual bool AddScriptFunc(const tString& asFuncDecl, void* pFunc)=0;
		
		/**
		 * Add a variable to the script vm. Example: "int MyVar"
		 * \param asVarDecl the declartion 
		 * \param *pVar the variable
		 * \return 
		 */
		virtual bool AddScriptVar(const tString& asVarDecl, void *pVar)=0;
		

	};
};
#endif // HPL_LOWLEVELSYSTEM_H
