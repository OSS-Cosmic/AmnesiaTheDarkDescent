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

#include "resources/FileSearcher.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/Platform.h"

#include "resources/LowLevelResources.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cFileSearcherEntry::cFileSearcherEntry(const tWString& asPath, int alPriority)
	{
		msPath = asPath;
		mlPriority = alPriority;
		m_mapScopePriorities[""] = alPriority;
        
		tWString sSepp = _W("/\\");
		cString::GetStringVecW(msPath,mvPathDirs,&sSepp);
	}

	//-----------------------------------------------------------------------


	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cFileSearcher::cFileSearcher()
	{
		msNull = _W("");
		mResolutionGeneration = 0;
	}

	//-----------------------------------------------------------------------

	cFileSearcher::~cFileSearcher()
	{
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	void cFileSearcher::AddDirectory(const tWString& asSearchPath, const tString &asMask, bool abAddSubDirectories, int alPriority, const tString& asScope)
	{
		//Make the path with only "/" and lower case.
		tWString sPath = cString::ReplaceCharToW(asSearchPath,_W("\\"),_W("/"));

		///////////////////////////////
		//Add all files in directory
		tWStringList lstFileNames;

		cPlatform::FindFilesInDir(lstFileNames,sPath, cString::To16Char(asMask));
			
		for(tWStringListIt it = lstFileNames.begin();it!=lstFileNames.end();it++)
		{
			tWString& sFile = *it;
			tString sLowFile = cString::ToLowerCase(cString::To8Char(sFile));
			tWString sFilePath = cString::ReplaceCharToW( cPlatform::GetFullFilePath( cString::SetFilePathW(sFile,sPath)), _W("\\"),_W("/"));;
			
			//Check if file and path already exist. The whole equivalent range is scanned:
			//another directory may have contributed a file of this bare name first, and find()
			//is not guaranteed to return the first element of the range (see GetFilePath).
			//Keep each scope's highest priority separately so removing an override
			//restores surviving registrations. Re-adds can raise a scope's priority;
			//only removing its contribution can lower the effective path priority.
			std::pair<tFilePathMapIt, tFilePathMapIt> range = m_mapFiles.equal_range(sLowFile);
			tFilePathMapIt pathIt = range.first;
			for(; pathIt != range.second; ++pathIt)
			{
				if(pathIt->second.msPath == sFilePath)
				{
					bool bChanged = false;
					std::map<tString, int>& mapScopes = pathIt->second.m_mapScopePriorities;
					std::map<tString, int>::iterator scopeIt = mapScopes.find(asScope);
					if(scopeIt == mapScopes.end())
					{
						mapScopes[asScope] = alPriority;
						bChanged = true;
					}
					else if(scopeIt->second < alPriority)
					{
						scopeIt->second = alPriority;
						bChanged = true;
					}

					if(pathIt->second.mlPriority < alPriority)
					{
						pathIt->second.mlPriority = alPriority;
						bChanged = true;
					}
					if(bChanged) ++mResolutionGeneration;
					break;
				}
			}
			if(pathIt != range.second)
			{
				continue;
			}

			//Add file
			//Log("Adding lowercase file: '%s' with path: '%s'\n 8bitHash: %u 16bitHash %u\n", sLowFile.c_str(), cString::To8Char(sFilePath).c_str(),
			//	cString::GetHash(cString::To8Char(sFilePath)), cString::GetHashW(sFilePath));
			cFileSearcherEntry entry(sFilePath, alPriority);
			if(!asScope.empty())
			{
				entry.m_mapScopePriorities.clear();
				entry.m_mapScopePriorities[asScope] = alPriority;
			}
			m_mapFiles.insert(tFilePathMap::value_type(sLowFile, entry));
			++mResolutionGeneration;
		}
		
		//////////////////////////////////
		//Search sub directories if set.
		if(abAddSubDirectories)
		{
			tWStringList lstDirNames;
			cPlatform::FindFoldersInDir(lstDirNames,sPath,false);
			
			for(tWStringListIt it = lstDirNames.begin();it!=lstDirNames.end();it++)
			{
				tWString sNewPath = cString::SetFilePathW(*it, sPath);

				AddDirectory(sNewPath,asMask,true,alPriority,asScope);
			}
		}
	}

	//-----------------------------------------------------------------------

	void cFileSearcher::RemoveDirectoryScope(const tString& asScope)
	{
		if(asScope.empty()) return;

		for(tFilePathMapIt it = m_mapFiles.begin(); it != m_mapFiles.end(); )
		{
			std::map<tString, int>& mapScopes = it->second.m_mapScopePriorities;
			if(mapScopes.find(asScope) == mapScopes.end())
			{
				++it;
				continue;
			}
			mapScopes.erase(asScope);
			++mResolutionGeneration;

			if(mapScopes.empty())
			{
				tFilePathMapIt eraseIt = it++;
				m_mapFiles.erase(eraseIt);
				continue;
			}

			int lEffectivePriority = mapScopes.begin()->second;
			for(std::map<tString, int>::const_iterator scopeIt = mapScopes.begin();
				scopeIt != mapScopes.end(); ++scopeIt)
			{
				if(scopeIt->second > lEffectivePriority)
					lEffectivePriority = scopeIt->second;
			}
			it->second.mlPriority = lEffectivePriority;
			++it;
		}
	}

	//-----------------------------------------------------------------------

	void cFileSearcher::ClearDirectories()
	{
		if(m_mapFiles.empty()) return;
		m_mapFiles.clear();
		++mResolutionGeneration;
	}

	//-----------------------------------------------------------------------

	size_t cFileSearcher::GetAllFilePaths(const tString& asFileName, tWStringVec& avPaths)
	{
		tString sLowName = cString::ToLowerCase(cString::GetFileName(asFileName));

		size_t lAdded =0;
		std::pair<tFilePathMapIt, tFilePathMapIt> range = m_mapFiles.equal_range(sLowName);
		for(tFilePathMapIt it = range.first; it != range.second; ++it)
		{
			avPaths.push_back(it->second.msPath);
			++lAdded;
		}

		return lAdded;
	}

	//-----------------------------------------------------------------------

	const tWString& cFileSearcher::GetFilePath(const tString& asFileNameAndPath, int *apEqualCount)
	{
		tString sFile = cString::GetFileName(asFileNameAndPath);
		tString sLowName = cString::ToLowerCase(sFile);

		//////////////////////
		//Get the iterator to path. equal_range is used because find() is not guaranteed to return the first element of the equivalent range.
		std::pair<tFilePathMapIt, tFilePathMapIt> range = m_mapFiles.equal_range(sLowName);
		if(range.first == range.second)
		{
			if(apEqualCount) *apEqualCount = 0;
			return msNull;
		}
		
		//////////////////////
		//If there is only one file with this name, just return it.
		//The range already answers that, so no second lookup is needed.
		tFilePathMapIt nextIt = range.first;
		++nextIt;
		if(nextIt == range.second && apEqualCount==NULL)
		{
			return range.first->second.msPath;
		}

		/////////////////////////////
		//Compare paths
		tWString sWantedPath = cString::To16Char(cString::GetFilePath(asFileNameAndPath));

		tWStringVec vWantedDirs;
		tWString sSepp =_W("/\\");
		
		int lBestEqualCount = 0;
		int lBestPriority = range.first->second.mlPriority;
		tFilePathMapIt bestEqualIt = range.first;
        
		cString::GetStringVecW(sWantedPath, vWantedDirs,&sSepp);

		//Iterate through equivalent entries and compare
		for(tFilePathMapIt it = range.first; it != range.second; ++it)
		{
			const tWStringVec& vCandidateDirs = it->second.mvPathDirs;

			//How well the candidate's directory chain matches the wanted one: the
			//larger of the two subsequence-match counts (wanted-driven and
			//candidate-driven).
			//Start with the wanted path dir
			int lEqualCount1 = 0;
			int j = (int)vCandidateDirs.size()-1;
			for(int i= (int)vWantedDirs.size()-1; (i>=0 && j>=0); --j)
			{
				//if equal, increase equal count and go to next wanted dir
				if(vWantedDirs[i] == vCandidateDirs[j])
				{
					lEqualCount1++;
					--i;
				}
			}

			//Start with the available path dir
			int lEqualCount2 = 0;
			j = (int)vWantedDirs.size()-1;
			for(int i= (int)vCandidateDirs.size()-1; (i>=0 && j>=0); --j)
			{
				//if equal, increase equal count and go to next wanted dir
				if(vCandidateDirs[i] == vWantedDirs[j])
				{
					lEqualCount2++;
					--i;
				}
			}

			int lMaxCount = lEqualCount1 > lEqualCount2 ? lEqualCount1 : lEqualCount2;

			/////////////////////
			//Resolution order when several indexed files share a bare filename:
			//  1. highest resource-dir priority wins outright -- an override dir
			//     shadows a shipped asset however well the shipped path matches,
			//     and whatever order resources.cfg happens to list the dirs in;
			//  2. then the best path-component match score;
			//  3. then the first-indexed candidate -- both comparisons are
			//     deliberately strict, so a tie leaves the incumbent in place and
			//     the first-indexed directory keeps winning.
			//Every dir defaults to klFileSearchDefaultPriority, so a config that
			//sets no Priority resolves exactly as it did before priorities existed.
			const bool bBetterCandidate =
				it->second.mlPriority != lBestPriority
					? it->second.mlPriority > lBestPriority
					: lMaxCount > lBestEqualCount;
			if(bBetterCandidate)
			{
				lBestEqualCount = lMaxCount;
				lBestPriority = it->second.mlPriority;
				bestEqualIt = it;
			}
		}

		if(apEqualCount) *apEqualCount = lBestEqualCount;

		//Return best fit
		return bestEqualIt->second.msPath;
	}

	//-----------------------------------------------------------------------

}
