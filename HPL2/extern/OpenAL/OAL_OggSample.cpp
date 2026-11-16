/**
	@file OAL_OggSample.cpp
	@author Luis Rodero
	@date 2006-10-02
	@version 0.1
	Derived class for containing Ogg Vorbis Sample data 
*/

#include "OpenAL/OAL_OggSample.h"
#include "OpenAL/OAL_Buffer.h"
#include "system/Platform.h"
#include "system/MemoryManager.h"
#include "system/LowLevelSystem.h"
#include <cstring>

//-------------------------------------------------------------------------------

extern ov_callbacks gCallbacks;

//-------------------------------------------------------------------------------

///////////////////////////////////////////////////////////
// PUBLIC METHODS
///////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------

bool cOAL_OggSample::CreateFromFile(const std::wstring &asFilename)
{
	DEF_FUNC_NAME("cOAL_OggSample::Load()");
	FUNC_USES_AL;

	if(mbStatus==false)
		return false;

	Reset();

	char *pPCMBuffer = NULL;
	bool bEOF = false;
	int lOpenResult;
	int lCurrent_section = 0;
	long lDataSize = 0;

	msFilename = asFilename;

	FILE *fileHandle = hpl::cPlatform::OpenFile(asFilename, L"rb");

	// If no file is present, set the error status and return
	if(!fileHandle)
	{
		mbStatus = false;
		return false;
	}

	// If not an Ogg file, set status and exit
	OggVorbis_File ovFileHandle;
	memset(&ovFileHandle, 0, sizeof(ovFileHandle));
	if((lOpenResult = ov_open_callbacks(fileHandle, &ovFileHandle, NULL, 0, gCallbacks)) < 0)
	{
		fclose ( fileHandle );
		mbStatus = false;
		return false;
	}

	// Get file info, rejecting anything we cannot represent as 16-bit mono/stereo
	vorbis_info *viFileInfo = ov_info ( &ovFileHandle, -1 );
	if(viFileInfo == NULL || viFileInfo->channels < 1 || viFileInfo->channels > 2 || viFileInfo->rate <= 0)
	{
		ov_clear(&ovFileHandle);
		mbStatus = false;
		return false;
	}
	mlChannels = viFileInfo->channels;
	mFormat = (mlChannels == 2)?AL_FORMAT_STEREO16:AL_FORMAT_MONO16;
	mlFrequency = viFileInfo->rate;
	mlSamples = (long) ov_pcm_total ( &ovFileHandle, -1 );
	mfTotalTime = ov_time_total( &ovFileHandle, -1 );

	if(mlSamples < 0)
	{
		ov_clear(&ovFileHandle);
		mbStatus = false;
		return false;
	}

	// Reserve memory for 'mlChannels' channels of 'mlSamples' * 2 bytes of data each.
	// ov_pcm_total can under-report on damaged files, so keep a safety margin and
	// never let ov_read write past the end of the buffer.
	const long long kSafetyMargin = STREAMING_BLOCK_SIZE * 8;
	const long long lSizeInBytes64 = (long long)(mlSamples > 0 ? mlSamples : 1) * mlChannels * GetBytesPerSample() + kSafetyMargin;
	if(lSizeInBytes64 <= 0 || lSizeInBytes64 > 0x7FFFFFFF)
	{
		ov_clear(&ovFileHandle);
		mbStatus = false;
		return false;
	}
	const long lSizeInBytes = (long)lSizeInBytes64;

	pPCMBuffer = (char *) hplMalloc (lSizeInBytes);
	if(pPCMBuffer == NULL)
	{
		ov_clear(&ovFileHandle);
		mbStatus = false;
		return false;
	}
	memset (pPCMBuffer, 0, lSizeInBytes);

	// Loop which loads chunks of decoded data into a buffer
	while(!bEOF)
	{
		long lRemaining = lSizeInBytes - lDataSize;
		if(lRemaining <= 0)
			break;

		long lChunkSize = ov_read ( &ovFileHandle			,
									pPCMBuffer + lDataSize	,
									(lRemaining < STREAMING_BLOCK_SIZE) ? (int)lRemaining : STREAMING_BLOCK_SIZE,
									SYS_ENDIANNESS			,
									2, 1, &lCurrent_section );

		if(lChunkSize == 0)
			bEOF = true;
		// If we get a negative value, then something went wrong. Clean up and set error status.
		else if(lChunkSize < 0)
		{
			hplFree(pPCMBuffer);
			ov_clear(&ovFileHandle);
			// ov_clear closes the file handle for us
			mbStatus = false;
			return false;
		}
		else
			lDataSize += lChunkSize;
	}

	cOAL_Buffer* pBuffer = mvBuffers[0];
	if(lDataSize > 0)
	{
		// If something went wrong, set error status. Clean up afterwards.
		mbStatus = pBuffer->Feed((ALvoid*)pPCMBuffer, lDataSize);
	}
	else
	{
		mbStatus = false;
	}
	hplFree(pPCMBuffer);
	ov_clear(&ovFileHandle);
	// ov_clear closes the file handle for us

	return mbStatus;
}

