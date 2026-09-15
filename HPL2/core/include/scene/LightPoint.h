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

	// Point light shape shared by the legacy and Redux point lights.
	class iLightPoint : public iLight
	{
	public:
		iLightPoint(tString asName, cResources *apResources);
	protected:
		void UpdateBoundingVolume() override;
	};

	// Retail point light (<PointLight>). Radius is where legacy attenuation ends;
	// rendered by the Standard renderer.
	class cLightPointLegacy : public iLightPoint
	{
	public:
		cLightPointLegacy(tString asName, cResources *apResources);
	};

	// Redux point light (<Re_PointLight>): Intensity, Radius (reach) and
	// SourceRadius feed the ray-traced light grid.
	class cLightPoint : public iLightPoint
	{
	public:
		cLightPoint(tString asName, cResources *apResources);
	};

};
#endif // HPL_LIGHT_POINT_H
