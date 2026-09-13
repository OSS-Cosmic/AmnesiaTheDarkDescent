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

#ifndef HPL_FILESEARCHER_H
#define HPL_FILESEARCHER_H

#include <map>
#include <cstdint>
#include "resources/ResourcesTypes.h"
#include "system/SystemTypes.h"

namespace hpl {

	class iLowLevelResources;

	//----------------------------------

	class cFileSearcherEntry
	{
	public:
		cFileSearcherEntry(const tWString& asPath, int alPriority = klFileSearchDefaultPriority);
			
		tWString msPath;
		tWStringVec mvPathDirs;
		int mlPriority;
		std::map<tString, int> m_mapScopePriorities;
	};

	//----------------------------------

	typedef std::multimap<tString, cFileSearcherEntry> tFilePathMap;
	typedef tFilePathMap::iterator tFilePathMapIt;

	//----------------------------------
	
	class cFileSearcher
	{
	public:
		cFileSearcher();
		~cFileSearcher();

		/**
		 * Adds a directory that will be searched when looking for files.
		 * \param asMask What files that should be searched for, for example: "*.jpeg".
		 * \param asPath The path to the directory.
		 * \param alPriority Priority assigned to files indexed from this directory.
		 * \param asScope Ownership key; empty means permanent until ClearDirectories.
		 * Re-adds keep the highest priority within each scope. Different scopes
		 * contribute independently even when they index the same exact path.
		 */
		void AddDirectory(const tWString& asSearchPath, const tString& asMask, bool abAddSubDirectories, int alPriority = klFileSearchDefaultPriority, const tString& asScope = "");

		/**
		 * Removes all indexed contributions belonging to a non-empty scope.
		 * Overlapping paths keep their other contributions and highest remaining
		 * priority. Empty or unknown scopes are no-ops. Existing resources are not freed.
		 */
		void RemoveDirectoryScope(const tString& asScope);

		/**
		 * Clears all directories
		 */
		void ClearDirectories();

		// Monotonic identity of indexed scope contributions. Actual additions,
		// priority raises, removals and non-empty clears advance it; repeated
		// no-op registrations do not. Existing resource handles are unaffected.
		uint64_t GetResolutionGeneration() const { return mResolutionGeneration; }

        /**
         * Gets a file pointer and searches through all added resources.
         * \param asName Name of the file.
		 * \return Path to the file. "" if file is not found.
         */
        const tWString& GetFilePath(const tString& asFileNameAndPath, int *apEqualCount=NULL);

		/**
		 * Gets every indexed path whose bare filename matches, in index order.
		 * Unlike GetFilePath (which scores candidates and returns the single best
		 * one) this returns all of them, so several resource dirs can each
		 * contribute a file of the same name -- used for stacking map/ent deltas.
		 * \return Number of paths appended to avPaths.
		 */
		size_t GetAllFilePaths(const tString& asFileName, tWStringVec& avPaths);

		/**
		 * The full index: lowercase filename -> entry (built at AddDirectory time).
		 */
		const tFilePathMap& GetAllFiles() const { return m_mapFiles; }

	private:
		tFilePathMap m_mapFiles;
		uint64_t mResolutionGeneration;

		tWString msNull;
	};

};
#endif // HPL_FILESEARCHER_H
