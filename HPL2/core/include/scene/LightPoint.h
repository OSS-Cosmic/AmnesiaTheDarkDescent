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

#ifndef HPL_LIGHT_POINT_H
#define HPL_LIGHT_POINT_H

#include "scene/Light.h"

namespace hpl {

	//------------------------------------------

	// A point light. It carries BOTH backends' tuning and resolves one per
	// query, so there is no separate legacy/ray-traced class any more.
	class cLightPoint : public iLight
	{
	public:
		cLightPoint(tString asName, cResources *apResources);
	protected:
		void UpdateBoundingVolume() override;
	};

	// The shape used to be split into an interface plus two classes; plenty of
	// code still spells the pointer this way.
	typedef cLightPoint iLightPoint;

};
#endif // HPL_LIGHT_POINT_H
