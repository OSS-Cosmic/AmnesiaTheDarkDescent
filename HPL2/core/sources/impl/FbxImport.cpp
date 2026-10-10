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

#include "impl/FbxImport.h"

#include "ufbx.h"

#include <cmath>
#include <cstring>
#include <map>
#include <set>

// The rules below follow the KFbx-based cMeshLoaderFBX that AMFP shipped with;
// the .msh/.anm caches in the retail install were produced by it. Where that
// loader had a plain bug, the fix is noted inline.
namespace hpl::fbx {

	namespace {

		struct Vec3 {
			float x, y, z;
			bool operator==(const Vec3& b) const { return x==b.x && y==b.y && z==b.z; }
			bool operator!=(const Vec3& b) const { return !(*this == b); }
		};

		struct Color { float r, g, b, a; };

		struct Vertex {
			Vec3 pos;
			Vec3 tex;
			Vec3 norm;
			Color col;
		};

		struct ExtraValue {
			int indexNum;	// position in the index list
			Vec3 tex;
		};

		//-----------------------------------------------------------------------

		Matrix ToHpl(const ufbx_matrix& m)
		{
			const float v[16] = {
				(float)m.m00, (float)m.m01, (float)m.m02, (float)m.m03,
				(float)m.m10, (float)m.m11, (float)m.m12, (float)m.m13,
				(float)m.m20, (float)m.m21, (float)m.m22, (float)m.m23,
				0, 0, 0, 1,
			};
			Matrix r;
			std::memcpy(r.m, v, sizeof(v));
			return r;
		}

		// KTime::GetMilliSeconds truncates the tick count and the old loader
		// keyed everything on whole milliseconds. The bias keeps exact frame
		// times (e.g. 0.1 s) from truncating a millisecond short.
		int64_t ToMilliSeconds(double afSeconds)
		{
			const double fMs = afSeconds * 1000.0;
			return (int64_t)(fMs + (fMs >= 0 ? 1e-6 : -1e-6));
		}

		// An attribute mapped per control point: ufbx indexes it with the
		// vertex indices. The old loader filled every control point from such
		// an attribute, including control points no face uses.
		template<typename T>
		bool IsByControlPoint(const T& aAttrib, const ufbx_mesh* apMesh)
		{
			if(aAttrib.values.count != apMesh->num_vertices) return false;
			if(aAttrib.indices.data == apMesh->vertex_indices.data) return true;
			for(size_t i=0; i<apMesh->num_indices; ++i)
				if(aAttrib.indices.data[i] != apMesh->vertex_indices.data[i]) return false;
			return true;
		}

		ufbx_matrix RotationMatrix(ufbx_quat aRot)
		{
			ufbx_transform t = {};
			t.rotation = aRot;
			t.scale = ufbx_vec3{1, 1, 1};
			return ufbx_transform_to_matrix(&t);
		}

		// T, R, S split the way KFbx's GetElements does it. A mirrored matrix
		// (negative determinant) gets all three scale axes negated, where
		// ufbx_matrix_to_transform negates one; the rotations then differ by
		// 180 degrees, which the retail caches show on mirrored end bones.
		ufbx_transform Decompose(const ufbx_matrix& m)
		{
			ufbx_transform t = {};
			t.translation = m.cols[3];

			ufbx_real vLen[3];
			for(int i=0; i<3; ++i)
				vLen[i] = std::sqrt(m.cols[i].x*m.cols[i].x + m.cols[i].y*m.cols[i].y + m.cols[i].z*m.cols[i].z);
			const ufbx_real fSign = ufbx_matrix_determinant(&m) < 0 ? -1 : 1;

			ufbx_matrix rot = ufbx_identity_matrix;
			for(int i=0; i<3; ++i)
			{
				const ufbx_real fS = vLen[i] > 0 ? fSign * vLen[i] : 1;
				rot.cols[i] = ufbx_vec3{ m.cols[i].x / fS, m.cols[i].y / fS, m.cols[i].z / fS };
			}
			t.rotation = ufbx_quat_normalize(ufbx_matrix_to_transform(&rot).rotation);
			t.scale = ufbx_vec3{ fSign * vLen[0], fSign * vLen[1], fSign * vLen[2] };
			return t;
		}

		Vec3 TexCoord(ufbx_vec2 aUv)
		{
			// Invert v, the engine uses the other convention.
			return Vec3{ (float)aUv.x, 1.0f - (float)aUv.y, 0 };
		}

		Vec3 ToVec3(ufbx_vec3 v) { return Vec3{ (float)v.x, (float)v.y, (float)v.z }; }

		Vec3 Normalized(Vec3 v)
		{
			const float fLen = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
			if(fLen <= 0) return v;
			return Vec3{ v.x/fLen, v.y/fLen, v.z/fLen };
		}

		std::string BaseNameNoExt(const char* apPath)
		{
			std::string s = apPath;
			const size_t slash = s.find_last_of("/\\");
			if(slash != std::string::npos) s = s.substr(slash + 1);
			const size_t dot = s.find_last_of('.');
			if(dot != std::string::npos) s = s.substr(0, dot);
			return s;
		}

		//-----------------------------------------------------------------------

		struct Importer {
			uint32_t flags = 0;
			Scene* out = nullptr;
			std::vector<ufbx_matrix> boneGlobal;			// bind pose, world space
			std::vector<ufbx_matrix> boneGlobalUnscaled;	// its rotation only
			std::map<std::string, int> boneIndex;

			void Warn(const std::string& asMsg) { out->warnings.push_back(asMsg); }

			int FindBone(const char* apName) const
			{
				auto it = boneIndex.find(apName);
				return it == boneIndex.end() ? -1 : it->second;
			}

			//-------------------------------------------------------------------

			void LoadSkeletonRec(const ufbx_node* apNode, int alParent)
			{
				if(apNode->bone)
				{
					Bone bone;
					bone.name = apNode->name.data;
					bone.parent = alParent;
					// Identity until a skin cluster links the bone. Unlinked bones
					// keep it, which puts them on their parent (as in retail).
					bone.local = ToHpl(ufbx_identity_matrix);
					bone.localUnscaled = bone.local;

					alParent = (int)out->skeleton.bones.size();
					boneIndex.emplace(bone.name, alParent);
					out->skeleton.bones.push_back(bone);
					boneGlobal.push_back(ufbx_identity_matrix);
					boneGlobalUnscaled.push_back(ufbx_identity_matrix);
				}

				for(size_t i=0; i<apNode->children.count; ++i)
					LoadSkeletonRec(apNode->children.data[i], alParent);
			}

			//-------------------------------------------------------------------

			void LoadSceneRec(const ufbx_node* apNode, bool abAnimationOnly)
			{
				if(apNode->mesh) LoadMeshData(apNode, abAnimationOnly);

				for(size_t i=0; i<apNode->children.count; ++i)
					LoadSceneRec(apNode->children.data[i], abAnimationOnly);
			}

			//-------------------------------------------------------------------

			void LoadMeshData(const ufbx_node* apNode, bool abAnimationOnly)
			{
				const ufbx_mesh* pMesh = apNode->mesh;
				const std::string sNodeName = apNode->name.data;
				// CON_ objects are animation controllers with no render data.
				const bool bController = sNodeName.compare(0, 4, "CON_") == 0;
				const bool bBuildMesh = !abAnimationOnly && !bController;

				std::vector<Vertex> vVertices;
				std::vector<uint32_t> vIndices;
				// Vertices created for a control point that needed a second uv,
				// so its bone weights can be copied to them.
				std::vector<std::vector<int>> vSplitVertices;
				SubMesh subMesh;

				if(bBuildMesh)
				{
					subMesh.name = sNodeName;
					const size_t lCpNum = pMesh->num_vertices;

					///////////////////////////////
					// Triangles: corner = index into the mesh's per-index arrays
					std::vector<uint32_t> vCorners;
					vCorners.reserve(pMesh->num_triangles * 3);
					std::vector<uint32_t> vTri(pMesh->max_face_triangles * 3);
					for(size_t f=0; f<pMesh->num_faces; ++f)
					{
						const ufbx_face face = pMesh->faces.data[f];
						if(face.num_indices == 3)
						{
							for(uint32_t k=0; k<3; ++k) vCorners.push_back(face.index_begin + k);
						}
						else if(face.num_indices > 3)
						{
							const uint32_t lTris = ufbx_triangulate_face(vTri.data(), vTri.size(), pMesh, face);
							vCorners.insert(vCorners.end(), vTri.begin(), vTri.begin() + lTris * 3);
						}
					}

					vIndices.resize(vCorners.size());
					for(size_t i=0; i<vCorners.size(); ++i)
						vIndices[i] = pMesh->vertex_indices.data[vCorners[i]];

					///////////////////////////////
					// Positions. tex.z == -1 marks "no uv assigned yet".
					vVertices.resize(lCpNum);
					for(size_t i=0; i<lCpNum; ++i)
					{
						vVertices[i].pos = ToVec3(pMesh->vertices.data[i]);
						vVertices[i].tex = Vec3{0, 0, -1};
						vVertices[i].norm = Vec3{0, 0, 0};
						vVertices[i].col = Color{1, 1, 1, 1};
					}
					vSplitVertices.resize(lCpNum);

					///////////////////////////////
					// Normals. Per-corner normals land on the control point (last
					// one wins); only uvs split vertices.
					if(pMesh->vertex_normal.exists)
					{
						if(IsByControlPoint(pMesh->vertex_normal, pMesh))
						{
							for(size_t i=0; i<lCpNum; ++i)
								vVertices[i].norm = ToVec3(pMesh->vertex_normal.values.data[i]);
						}
						else
						{
							for(size_t i=0; i<vCorners.size(); ++i)
								vVertices[vIndices[i]].norm = ToVec3(ufbx_get_vertex_vec3(&pMesh->vertex_normal, vCorners[i]));
						}
					}

					///////////////////////////////
					// UV coords
					const bool bHasUv = pMesh->vertex_uv.exists;
					std::vector<ExtraValue> vExtraValues;
					if(bHasUv)
					{
						if(IsByControlPoint(pMesh->vertex_uv, pMesh))
						{
							for(size_t i=0; i<lCpNum; ++i)
								vVertices[i].tex = TexCoord(pMesh->vertex_uv.values.data[i]);
						}
						else
						{
							for(size_t i=0; i<vCorners.size(); ++i)
							{
								const Vec3 vTex = TexCoord(ufbx_get_vertex_vec2(&pMesh->vertex_uv, vCorners[i]));
								Vertex& vtx = vVertices[vIndices[i]];

								//A control point that already has another uv needs a new vertex.
								if(vtx.tex.z != -1 && vtx.tex != vTex)
									vExtraValues.push_back(ExtraValue{ (int)i, vTex });
								else
									vtx.tex = vTex;
							}
						}

						///////////////////////////////
						// Add the extra vertices, reusing one already added for the
						// same control point and uv.
						const size_t lStartPos = vVertices.size();
						for(const ExtraValue& val : vExtraValues)
						{
							const uint32_t lVtxNum = vIndices[val.indexNum];
							const Vertex& src = vVertices[lVtxNum];

							bool bOldFound = false;
							for(size_t i=lStartPos; i<vVertices.size(); ++i)
							{
								if(vVertices[i].tex == val.tex && vVertices[i].pos == src.pos && vVertices[i].norm == src.norm)
								{
									vIndices[val.indexNum] = (uint32_t)i;
									bOldFound = true;
									break;
								}
							}
							if(bOldFound) continue;

							Vertex newVtx = src;	// the old loader left the colour black here
							newVtx.tex = val.tex;
							vSplitVertices[lVtxNum].push_back((int)vVertices.size());
							vVertices.push_back(newVtx);
							vIndices[val.indexNum] = (uint32_t)vVertices.size() - 1;
						}
					}

					///////////////////////////////
					// Material
					if(pMesh->materials.count > 0)
					{
						const ufbx_material* pMat = pMesh->face_material.count > 0
							? pMesh->materials.data[pMesh->face_material.data[0]]
							: pMesh->materials.data[0];

						if(pMesh->materials.count > 1)
						{
							for(size_t f=1; f<pMesh->face_material.count; ++f)
							{
								if(pMesh->face_material.data[f] != pMesh->face_material.data[0])
								{
									// The old loader dropped the whole mesh here.
									Warn("'" + sNodeName + "' uses more than one material; using '" + std::string(pMat->name.data) + "' for all of it");
									break;
								}
							}
						}

						subMesh.material = pMat->name.data;

						if(pMat->shader_type == UFBX_SHADER_FBX_PHONG)
						{
							const ufbx_vec4 col = pMat->fbx.diffuse_color.value_vec4;
							const float fFactor = (float)pMat->fbx.diffuse_factor.value_real;
							for(Vertex& v : vVertices)
								v.col = Color{ (float)col.x, (float)col.y, (float)col.z, fFactor };
						}

						// The old loader let a layer texture override this, but the
						// SDK it used never exposed material-linked (FBX 7) textures
						// that way, so retail always kept the material name. Fall
						// back to the diffuse texture's file only for unnamed ones.
						if(subMesh.material.empty() && bHasUv && pMat->fbx.diffuse_color.texture)
						{
							const ufbx_texture* pTex = pMat->fbx.diffuse_color.texture;
							const ufbx_string& sFile = pTex->relative_filename.length > 0 ? pTex->relative_filename : pTex->filename;
							if(sFile.length > 0) subMesh.material = BaseNameNoExt(sFile.data);
						}
					}

					///////////////////////////////
					// Transform. Vertices go into the node's world space (without
					// the geometric transform); normals use the local 3x3.
					const ufbx_matrix& mtxGlobal = apNode->node_to_world;
					const ufbx_matrix& mtxLocal = apNode->node_to_parent;
					for(Vertex& v : vVertices)
					{
						v.pos = ToVec3(ufbx_transform_position(&mtxGlobal, ufbx_vec3{ v.pos.x, v.pos.y, v.pos.z }));
						v.norm = Normalized(ToVec3(ufbx_transform_direction(&mtxLocal, ufbx_vec3{ v.norm.x, v.norm.y, v.norm.z })));
					}

					// Flip the winding.
					for(size_t i=0; i+2<vIndices.size(); i+=3)
						std::swap(vIndices[i], vIndices[i+2]);
				}

				///////////////////////////////
				// Skin: bind poses for the bones, and the vertex weights.
				if(out->hasSkeleton)
				{
					if(pMesh->skin_deformers.count > 1)
						Warn("'" + sNodeName + "' has more than one skin deformer");

					for(size_t d=0; d<pMesh->skin_deformers.count; ++d)
					{
						const ufbx_skin_deformer* pSkin = pMesh->skin_deformers.data[d];
						for(size_t c=0; c<pSkin->clusters.count; ++c)
						{
							const ufbx_skin_cluster* pCluster = pSkin->clusters.data[c];
							if(pCluster->bone_node == nullptr) continue;

							const int lBone = FindBone(pCluster->bone_node->name.data);
							if(lBone < 0)
							{
								Warn("'" + sNodeName + "' is skinned to '" + std::string(pCluster->bone_node->name.data) + "', which is not a skeleton node");
								continue;
							}

							// TransformLink times the bone's geometric transform,
							// rebuilt from T, R and S (shear is dropped).
							const ufbx_matrix mtxLink = ufbx_matrix_mul(&pCluster->bind_to_world, &pCluster->bone_node->geometry_to_node);
							const ufbx_transform trs = Decompose(mtxLink);
							boneGlobal[lBone] = ufbx_transform_to_matrix(&trs);
							boneGlobalUnscaled[lBone] = RotationMatrix(trs.rotation);
							out->skeleton.bones[lBone].linked = true;

							if(!bBuildMesh) continue;

							for(size_t w=0; w<pCluster->num_weights; ++w)
							{
								const uint32_t lCp = pCluster->vertices.data[w];
								const float fWeight = (float)pCluster->weights.data[w];
								subMesh.bonePairs.push_back(VertexBonePair{ (int)lCp, lBone, fWeight });

								// The old loader never pushed these, leaving split
								// vertices unskinned.
								if(lCp < vSplitVertices.size())
								{
									for(int lSplit : vSplitVertices[lCp])
										subMesh.bonePairs.push_back(VertexBonePair{ lSplit, lBone, fWeight });
								}
							}
						}
					}
				}

				if(!bBuildMesh) return;

				///////////////////////////////
				// Output
				const size_t lVtxNum = vVertices.size();
				subMesh.positions.resize(lVtxNum * 3);
				subMesh.normals.resize(lVtxNum * 3);
				subMesh.texcoords.resize(lVtxNum * 3);
				subMesh.colors.resize(lVtxNum * 4);
				for(size_t i=0; i<lVtxNum; ++i)
				{
					const Vertex& v = vVertices[i];
					// Control points without a uv keep the (0,0,-1) marker in the
					// old loader; an unused marker is not a real coordinate.
					const Vec3 vTex = v.tex.z == -1 ? Vec3{0, 0, 0} : v.tex;
					std::memcpy(&subMesh.positions[i*3], &v.pos, sizeof(float)*3);
					std::memcpy(&subMesh.normals[i*3], &v.norm, sizeof(float)*3);
					std::memcpy(&subMesh.texcoords[i*3], &vTex, sizeof(float)*3);
					std::memcpy(&subMesh.colors[i*4], &v.col, sizeof(float)*4);
				}
				subMesh.indices = std::move(vIndices);
				out->subMeshes.push_back(std::move(subMesh));
			}

			//-------------------------------------------------------------------

			// Linked bones get local = inv(parent global) * global. Unlinked bones
			// keep identity and pass their parent's global on to their children.
			void MakeFinalBones()
			{
				std::vector<ufbx_matrix> vEffective(out->skeleton.bones.size());
				std::vector<ufbx_matrix> vEffectiveUnscaled(out->skeleton.bones.size());

				for(size_t i=0; i<out->skeleton.bones.size(); ++i)
				{
					Bone& bone = out->skeleton.bones[i];
					const ufbx_matrix mtxParent = bone.parent >= 0 ? vEffective[bone.parent] : ufbx_identity_matrix;
					const ufbx_matrix mtxParentUnscaled = bone.parent >= 0 ? vEffectiveUnscaled[bone.parent] : ufbx_identity_matrix;

					if(bone.linked)
					{
						const ufbx_matrix mtxInvParent = ufbx_matrix_invert(&mtxParent);
						const ufbx_matrix mtxInvParentUnscaled = ufbx_matrix_invert(&mtxParentUnscaled);
						const ufbx_matrix mtxLocal = ufbx_matrix_mul(&mtxInvParent, &boneGlobal[i]);
						const ufbx_matrix mtxLocalUnscaled = ufbx_matrix_mul(&mtxInvParentUnscaled, &boneGlobalUnscaled[i]);
						bone.local = ToHpl(mtxLocal);
						bone.localUnscaled = ToHpl(mtxLocalUnscaled);
						vEffective[i] = boneGlobal[i];
						vEffectiveUnscaled[i] = boneGlobalUnscaled[i];
					}
					else
					{
						vEffective[i] = mtxParent;
						vEffectiveUnscaled[i] = mtxParentUnscaled;
					}
				}
			}

			//-------------------------------------------------------------------

			void LoadAnimations(const ufbx_scene* apScene)
			{
				// The first take with a non-empty time span.
				const ufbx_anim_stack* pStack = nullptr;
				int64_t lEnd = 0;
				for(size_t i=0; i<apScene->anim_stacks.count; ++i)
				{
					const ufbx_anim_stack* pCandidate = apScene->anim_stacks.data[i];
					const int64_t lStart = ToMilliSeconds(pCandidate->time_begin);
					lEnd = ToMilliSeconds(pCandidate->time_end);
					if(lEnd - lStart > 0) { pStack = pCandidate; break; }
				}
				if(pStack == nullptr || pStack->layers.count == 0) return;

				out->hasAnimation = true;
				Animation& anim = out->animation;
				anim.name = pStack->name.data;
				anim.length = (float)lEnd / 1000.0f;

				LoadAnimationRec(apScene->root_node, pStack, anim);
			}

			void LoadAnimationRec(const ufbx_node* apNode, const ufbx_anim_stack* apStack, Animation& aAnim)
			{
				const int lBone = apNode->bone ? FindBone(apNode->name.data) : -1;
				if(lBone >= 0)
				{
					const ufbx_anim_layer* pLayer = apStack->layers.data[0];

					// Key times of every T/R/S channel, in whole milliseconds.
					std::set<float> setTimes;
					const char* vProps[] = { UFBX_Lcl_Translation, UFBX_Lcl_Rotation, UFBX_Lcl_Scaling };
					for(const char* sProp : vProps)
					{
						const ufbx_anim_prop* pProp = ufbx_find_anim_prop(pLayer, &apNode->element, sProp);
						if(pProp == nullptr || pProp->anim_value == nullptr) continue;
						for(const ufbx_anim_curve* pCurve : pProp->anim_value->curves)
						{
							if(pCurve == nullptr) continue;
							for(size_t k=0; k<pCurve->keyframes.count; ++k)
								setTimes.insert((float)ToMilliSeconds(pCurve->keyframes.data[k].time) / 1000.0f);
						}
					}

					const Bone& bone = out->skeleton.bones[lBone];
					const float vRestTrans[3] = { bone.local.m[3], bone.local.m[7], bone.local.m[11] };

					// Rotations are stored relative to the rest pose:
					// key = inv(rest rotation) * evaluated rotation.
					ufbx_matrix mtxRestRot = ufbx_identity_matrix;
					mtxRestRot.m00 = bone.localUnscaled.m[0]; mtxRestRot.m01 = bone.localUnscaled.m[1]; mtxRestRot.m02 = bone.localUnscaled.m[2];
					mtxRestRot.m10 = bone.localUnscaled.m[4]; mtxRestRot.m11 = bone.localUnscaled.m[5]; mtxRestRot.m12 = bone.localUnscaled.m[6];
					mtxRestRot.m20 = bone.localUnscaled.m[8]; mtxRestRot.m21 = bone.localUnscaled.m[9]; mtxRestRot.m22 = bone.localUnscaled.m[10];
					ufbx_quat qInvRest = Decompose(mtxRestRot).rotation;
					qInvRest.x = -qInvRest.x; qInvRest.y = -qInvRest.y; qInvRest.z = -qInvRest.z;
					qInvRest = ufbx_quat_normalize(qInvRest);

					Track track;
					track.name = apNode->name.data;
					track.flags = kAnimFlag_Translate | kAnimFlag_Rotate;
					track.keys.reserve(setTimes.size());

					for(float fTime : setTimes)
					{
						// The old loader went through a millisecond KTime here too.
						const double fSeconds = (double)(int64_t)(fTime * 1000) / 1000.0;
						const ufbx_transform localTrs = ufbx_evaluate_transform(apStack->anim, apNode, fSeconds);
						const ufbx_matrix mtxLocalAnim = ufbx_transform_to_matrix(&localTrs);
						const ufbx_matrix mtxLocal = ufbx_matrix_mul(&mtxLocalAnim, &apNode->geometry_to_node);
						const ufbx_transform trs = Decompose(mtxLocal);

						ufbx_quat qRot = ufbx_quat_normalize(ufbx_quat_mul(qInvRest, ufbx_quat_normalize(trs.rotation)));
						// Keep neighbouring keys in the same hemisphere so they
						// interpolate the short way round.
						if(!track.keys.empty())
						{
							const float* p = track.keys.back().rot;
							if(p[0]*qRot.x + p[1]*qRot.y + p[2]*qRot.z + p[3]*qRot.w < 0)
							{
								qRot.x = -qRot.x; qRot.y = -qRot.y; qRot.z = -qRot.z; qRot.w = -qRot.w;
							}
						}

						KeyFrame key;
						key.time = fTime;
						key.trans[0] = (float)trs.translation.x - vRestTrans[0];
						key.trans[1] = (float)trs.translation.y - vRestTrans[1];
						key.trans[2] = (float)trs.translation.z - vRestTrans[2];
						key.rot[0] = (float)qRot.x;
						key.rot[1] = (float)qRot.y;
						key.rot[2] = (float)qRot.z;
						key.rot[3] = (float)qRot.w;
						track.keys.push_back(key);
					}

					aAnim.tracks.push_back(std::move(track));
				}

				for(size_t i=0; i<apNode->children.count; ++i)
					LoadAnimationRec(apNode->children.data[i], apStack, aAnim);
			}
		};
	}

	//-----------------------------------------------------------------------

	bool Import(const std::string& asPath, uint32_t aFlags, Scene& aOut, std::string& asError)
	{
		ufbx_load_opts opts = {};
		// No axis or unit conversion: the old loader used raw file space, and
		// the retail caches (and every placement built on them) are in it.
		ufbx_error error;
		ufbx_scene* pScene = ufbx_load_file(asPath.c_str(), &opts, &error);
		if(pScene == nullptr)
		{
			asError = std::string(error.description.data, error.description.length);
			return false;
		}

		aOut = Scene();
		Importer importer;
		importer.flags = aFlags;
		importer.out = &aOut;

		importer.LoadSkeletonRec(pScene->root_node, -1);
		aOut.hasSkeleton = !aOut.skeleton.bones.empty();

		importer.LoadSceneRec(pScene->root_node, (aFlags & ImportFlag_Meshes) == 0);

		if(aOut.hasSkeleton) importer.MakeFinalBones();

		if(aFlags & ImportFlag_Animation) importer.LoadAnimations(pScene);

		ufbx_free_scene(pScene);
		return true;
	}
}
