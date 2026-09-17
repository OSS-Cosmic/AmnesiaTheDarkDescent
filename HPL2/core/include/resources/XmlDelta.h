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

#ifndef HPL_XML_DELTA_H
#define HPL_XML_DELTA_H

#include "system/SystemTypes.h"

namespace tinyxml2 {
	class XMLElement;
	class XMLDocument;
}

namespace hpl {

	//-------------------------------------
	// Delta ("patch") application for the XML documents the engine loads:
	// .map files (via .map_delta) and .ent files (via .ent_delta).
	//
	// A delta is a small XML document listing Add / Remove / Modify operations
	// against objects in the base document, keyed by their "ID" attribute. It is
	// applied to the parsed tinyxml2 tree in memory, between parsing and walking,
	// so no per-object loader needs to know deltas exist. Base files on disk are
	// never modified.
	//
	//   <MapDelta Version="1" Target="maps/x/y.map" Name="my_mod" Priority="100">
	//     <SetMapData FogActive="true" FogEnd="30"/>
	//     <Remove Category="StaticObjects" ID="42"/>
	//     <Modify Category="Entities" ID="339" Name="hatch_ceiling_1">
	//       <SetAttr WorldPos="53 8 30.25" Active="false"/>
	//       <RemoveAttr Name="Tag"/>
	//       <SetVar Name="Locked" Value="true"/>
	//       <RemoveVar Name="PlayerInteractCallback"/>
	//     </Modify>
	//     <Add Category="Entities">
	//       <Entity Name="mymod_lamp_1" Filename="entities/lights/lamp.ent" .../>
	//     </Add>
	//     <Add Category="Entities" After="12">   (inserted behind object 12)
	//       <Entity Name="mymod_lamp_2" Filename="entities/lights/lamp.ent" .../>
	//     </Add>
	//   </MapDelta>
	//
	// Retuning a light for the ray-traced renderer is a plain <Modify>: the
	// retail attributes stay as they are and the overrides ride along with a
	// "Re_" prefix (see LightParameters.h), so the light remains ONE object.
	//
	//   <Modify Category="Entities" ID="11" Name="PointLight_1">
	//     <SetAttr Re_Intensity="1.3" Re_Radius="10" Re_SourceRadius="0.035"/>
	//   </Modify>
	//
	// An <Add>ed object may reuse an existing name only when it carries an
	// explicit RendererMask and no renderer it loads for then has the name more
	// often than the base file did -- the other half of an object split by
	// backend, e.g. a retail object narrowed to RendererMask="1" by a <Modify>
	// placed before the <Add> of its RendererMask="2" replacement. Added objects
	// a later <Modify> makes overlap again are dropped.
	//
	// The .ent form is identical with an <EntDelta> root; its categories are the
	// <ModelData> children (Mesh/Bones/Shapes/Bodies/Joints/Animations).
	//-------------------------------------

	//----------------------------------

	// First ID handed out to <Add>ed objects. Kept well above anything the editor
	// allocates (a dense counter from 0) so added objects never collide with base
	// IDs, which save games persist entity state against.
	#define HPL_XML_DELTA_FIRST_ADD_ID 1000000

	//----------------------------------

	class cXmlDeltaStats
	{
	public:
		cXmlDeltaStats() : mlAdded(0), mlModified(0), mlRemoved(0), mlSkipped(0) {}

		int mlAdded;
		int mlModified;
		int mlRemoved;
		int mlSkipped;

		bool IsEmpty() const { return mlAdded==0 && mlModified==0 && mlRemoved==0; }

		void Add(const cXmlDeltaStats& aX)
		{
			mlAdded += aX.mlAdded; mlModified += aX.mlModified;
			mlRemoved += aX.mlRemoved; mlSkipped += aX.mlSkipped;
		}
	};

	//----------------------------------

	/**
	 * Applies one delta document to a parsed base document, in place.
	 *
	 * \param apTargetRoot The element holding the object categories: <MapData> for
	 *                     a map (its <MapContents> child is used), or <Entity> for
	 *                     an .ent (its <ModelData> child is used).
	 * \param apDeltaRoot  The <MapDelta> / <EntDelta> root element.
	 * \param alNextAddID  In/out. Next ID to hand out to <Add>ed objects; advanced
	 *                     as they are consumed so several deltas stacked on one map
	 *                     get disjoint ranges. Seed with HPL_XML_DELTA_FIRST_ADD_ID.
	 * \param aStats       Out. Operation counts, for logging.
	 * \return false only if the documents are structurally unusable. Individual bad
	 *         operations warn, increment mlSkipped and are ignored.
	 */
	bool ApplyXmlDelta(	tinyxml2::XMLElement* apTargetRoot,
						tinyxml2::XMLElement* apDeltaRoot,
						int& alNextAddID,
						cXmlDeltaStats& aStats);

	/**
	 * Priority of a delta document, used to order stacked deltas. Missing = 0.
	 */
	int GetXmlDeltaPriority(tinyxml2::XMLElement* apDeltaRoot);

	/**
	 * FNV-1a 64 over the raw bytes of each file, in the order given. Identifies a
	 * set of deltas so derived artifacts (the .map_cache) can be keyed on it.
	 * Returns 0 for an empty list.
	 */
	unsigned long long HashFileSet(const tWStringVec& avFiles);

	//----------------------------------
};
#endif // HPL_XML_DELTA_H
