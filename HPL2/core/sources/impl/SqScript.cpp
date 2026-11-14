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

#include "impl/SqScript.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/Platform.h"
#include "math/Math.h"
#include <stdio.h>
#include "impl/scripthelper.h"
#ifndef AMFP
#include "impl/scriptstring.h"
#endif
#include "resources/BinaryBuffer.h"
#include "resources/Resources.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC DATA
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	#define kEncryptKey 0x4516FFDD

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cSqScript::cSqScript(const tString& asName,asIScriptEngine *apScriptEngine,
							cScriptOutput *apScriptOutput, int alHandle)
		: iScript(asName, _W(""))
	{
		mpScriptEngine = apScriptEngine;
		mpScriptOutput = apScriptOutput;
		mlHandle = alHandle;

		mpContext = mpScriptEngine->CreateContext();

		//Create a unique module name
		msModuleName = "Module_"+cString::ToString(cMath::RandRectl(0,1000000))+
						"_"+cString::ToString(mlHandle);

	}

	cSqScript::~cSqScript()
	{
		mpScriptEngine->DiscardModule(msModuleName.c_str());
		mpContext->Release();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	bool cSqScript::CreateFromFile(const tWString& asFileName, tString *apCompileMessages)
	{
		SetFullPath(asFileName);

		tWString sExt = cString::ToLowerCaseW(cString::GetFileExtW(asFileName));

		/////////////////////////////////////////
		// Load file
		int lLength = 0;
		char *pCharBuffer = NULL;
		
		/////////////////////////////////////
		// Normal load
		if(sExt == _W("hps"))
		{
			pCharBuffer = LoadCharBuffer(asFileName,lLength);
			if(pCharBuffer==NULL){
				Error("Couldn't load script '%s'!\n",asFileName.c_str());
				return false;
			}
		}
		/////////////////////////////////////
		// Compressed load
		else if(cResources::GetCreateAndLoadCompressedMaps())
		{
			cBinaryBuffer compBuffer;
			if(compBuffer.Load(asFileName)==false)
			{
				//Log("Could not load compressed map!\n");
				return false;
			}

			int lKey = kEncryptKey;
			compBuffer.XorTransform((char*)&lKey, sizeof(lKey));

			cBinaryBuffer textBuff;
			if(textBuff.DecompressAndAddFromBuffer(&compBuffer, false)==false)
			{
				//Log("Could not decompress map!\n");
				return false;
			}

			textBuff.SetPos(0);
			lLength = (int)textBuff.GetSize();
			pCharBuffer = hplNewArray(char,lLength);
			textBuff.GetCharArray(pCharBuffer, lLength);
		}

		/////////////////////////////////////////
		// Save compressed
		if(sExt == _W("hps") && cResources::GetCreateAndLoadCompressedMaps())
		{
			tWString sCompFile = cString::SetFileExtW(asFileName,_W("chps"));

			//Only recreate if file does not exist or if out of date.
			if(	cPlatform::FileExists(sCompFile)==false || 
				cPlatform::FileModifiedDate(sCompFile) < cPlatform::FileModifiedDate(asFileName))
			{
				cBinaryBuffer textBuff;
				textBuff.AddCharArray(pCharBuffer, lLength);

				cBinaryBuffer compBuff;
				compBuff.CompressAndAdd(textBuff.GetDataPointer(), textBuff.GetSize());

				int lKey = kEncryptKey;
				compBuff.XorTransform((char*)&lKey, sizeof(lKey));

				compBuff.Save(sCompFile);
			}
		}
		
		/////////////////////////////////////////
		// Create module
		mpModule = mpScriptEngine->GetModule(msModuleName.c_str(), asGM_ALWAYS_CREATE);
		if(mpModule->AddScriptSection("main", pCharBuffer, lLength)<0)
		{
			Error("Couldn't add script '%s'!\n",asFileName.c_str());
			hplDeleteArray(pCharBuffer);
			return false;
		}

		int lBuildOutput = mpModule->Build();
		if(apCompileMessages) *apCompileMessages = mpScriptOutput->GetMessage();

		if(lBuildOutput<0)
		{
			Error("Couldn't build script '%s'!\n",cString::To8Char(asFileName).c_str());
			Log("------- SCRIPT OUTPUT BEGIN --------------------------\n");
			mpScriptOutput->Display();
			mpScriptOutput->Clear();
			Log("------- SCRIPT OUTPUT END ----------------------------\n");
			
			hplDeleteArray(pCharBuffer);
			return false;
		}
		mpScriptOutput->Clear();

		hplDeleteArray(pCharBuffer);
		return true;
	}

	//-----------------------------------------------------------------------

	int cSqScript::GetFuncHandle(const tString& asFunc)
	{
#ifdef AMFP
		// AngelScript 2.24 hides the id-based API behind AS_DEPRECATED.
		asIScriptFunction *pFunc = mpModule->GetFunctionByName(asFunc.c_str());
		return pFunc ? pFunc->GetId() : -1;
#else
		return mpModule->GetFunctionIdByName(asFunc.c_str());
#endif
	}

	//-----------------------------------------------------------------------

	void cSqScript::AddArg(const tString& asArg)
	{

	}

	//-----------------------------------------------------------------------

	bool cSqScript::Run(const tString& asFuncLine)
	{
		ExecuteString(mpScriptEngine, asFuncLine.c_str(), mpModule);

		return true;
	}

	//-----------------------------------------------------------------------

	bool cSqScript::Run(int alHandle)
	{
#ifdef AMFP
		mpContext->Prepare(mpScriptEngine->GetFunctionById(alHandle));
#else
		mpContext->Prepare(alHandle);
#endif

		/* Set all the args here */

		mpContext->Execute();

		return true;
	}

	//-----------------------------------------------------------------------

	bool cSqScript::RunFuncString(const tString& asFuncName, const tString& asArg0)
	{
		if(PrepareSingleArgFunc(asFuncName, mpScriptEngine->GetTypeIdByDecl("string"))==false)
			return false;

#ifdef AMFP
		// AMFP registers std::string as the script string type
		tString sArg = asArg0;
		mpContext->SetArgObject(0, &sArg);
		return ExecutePrepared(asFuncName);
#else
		// TDD registers the ref counted CScriptString as the script string type
		CScriptString *pArg = new CScriptString(asArg0);
		mpContext->SetArgObject(0, pArg);
		bool bRet = ExecutePrepared(asFuncName);
		pArg->Release();
		return bRet;
#endif
	}

	//-----------------------------------------------------------------------

	bool cSqScript::RunFuncFloat(const tString& asFuncName, float afArg0)
	{
		if(PrepareSingleArgFunc(asFuncName, asTYPEID_FLOAT)==false)
			return false;

		mpContext->SetArgFloat(0, afArg0);
		return ExecutePrepared(asFuncName);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	bool cSqScript::PrepareSingleArgFunc(const tString& asFuncName, int alArgTypeId)
	{
		if(mpModule==NULL || mpContext==NULL || alArgTypeId < 0) return false;

		// Nested call (a script is already running on this context): let the caller fall back
		if(mpContext->GetState() == asEXECUTION_ACTIVE) return false;

#ifdef AMFP
		// Returns NULL when there is no function or several overloads with this name
		asIScriptFunction *pFunc = mpModule->GetFunctionByName(asFuncName.c_str());
#else
		int lFuncId = mpModule->GetFunctionIdByName(asFuncName.c_str());
		asIScriptFunction *pFunc = lFuncId >= 0 ? mpScriptEngine->GetFunctionDescriptorById(lFuncId) : NULL;
#endif
		if(pFunc==NULL) return false;
		if(pFunc->GetParamCount() != 1) return false;
		if(pFunc->GetParamTypeId(0) != alArgTypeId) return false;

#ifdef AMFP
		return mpContext->Prepare(pFunc) >= 0;
#else
		return mpContext->Prepare(lFuncId) >= 0;
#endif
	}

	//-----------------------------------------------------------------------

	bool cSqScript::ExecutePrepared(const tString& asFuncName)
	{
		int lRet = mpContext->Execute();
		if(lRet == asEXECUTION_EXCEPTION)
		{
			Error("Script exception in '%s' (%s): %s\n", asFuncName.c_str(), msModuleName.c_str(),
				  mpContext->GetExceptionString() ? mpContext->GetExceptionString() : "");
		}
		// The function was found and run, even if it threw, so callers must not run it again.
		return true;
	}

	//-----------------------------------------------------------------------

	char* cSqScript::LoadCharBuffer(const tWString& asFileName, int& alLength)
	{
		FILE *pFile = cPlatform::OpenFile(asFileName, _W("rb"));
		if(pFile==NULL){
			return NULL;
		}

		fseek(pFile,0,SEEK_END);
		int lLength = (int)ftell(pFile);
		rewind(pFile);
		
		alLength = lLength;

		char *pBuffer = hplNewArray(char,lLength);
		fread(pBuffer, lLength, 1, pFile);

		fclose(pFile);

		return pBuffer;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// STATIC PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	//-----------------------------------------------------------------------

}
