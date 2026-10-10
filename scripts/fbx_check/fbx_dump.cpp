// Dumps hpl::fbx::Import output as JSON: fbx_dump <file.fbx> mesh|anim
#include "impl/FbxImport.h"
#include <cstdio>
#include <string>
using namespace hpl::fbx;
static void pf(const float* p, int n){ printf("["); for(int i=0;i<n;i++) printf("%s%.7g", i?",":"", p[i]); printf("]"); }
static void pstr(const std::string& s){ printf("\""); for(char c: s){ if(c=='"'||c=='\\') printf("\\%c",c); else printf("%c",c);} printf("\""); }
int main(int, char** argv){
  std::string err; Scene sc; uint32_t fl = std::string(argv[2])=="anim" ? ImportFlag_Animation : ImportFlag_Meshes;
  if(!Import(argv[1], fl, sc, err)){ printf("{\"error\":"); pstr(err); printf("}\n"); return 0; }
  printf("{\"bones\":[");
  for(size_t i=0;i<sc.skeleton.bones.size();i++){ auto& b=sc.skeleton.bones[i]; printf("%s{\"name\":",i?",":""); pstr(b.name); printf(",\"parent\":%d,\"linked\":%d,\"m\":",b.parent,(int)b.linked); pf(b.local.m,16); printf("}"); }
  printf("],\"subs\":[");
  for(size_t i=0;i<sc.subMeshes.size();i++){ auto& s=sc.subMeshes[i]; printf("%s{\"name\":",i?",":""); pstr(s.name); printf(",\"material\":"); pstr(s.material);
    printf(",\"pos\":"); pf(s.positions.data(), s.positions.size()); printf(",\"nrm\":"); pf(s.normals.data(), s.normals.size());
    printf(",\"uv\":"); pf(s.texcoords.data(), s.texcoords.size()); printf(",\"col\":"); pf(s.colors.data(), s.colors.size());
    printf(",\"idx\":["); for(size_t k=0;k<s.indices.size();k++) printf("%s%u",k?",":"",s.indices[k]); printf("]");
    printf(",\"pairs\":["); for(size_t k=0;k<s.bonePairs.size();k++) printf("%s[%d,%d,%.7g]",k?",":"",s.bonePairs[k].vtx,s.bonePairs[k].bone,s.bonePairs[k].weight); printf("]}"); }
  printf("],\"anim\":");
  if(!sc.hasAnimation) printf("null"); else { printf("{\"name\":"); pstr(sc.animation.name); printf(",\"length\":%.7g,\"tracks\":[", sc.animation.length);
    for(size_t t=0;t<sc.animation.tracks.size();t++){ auto& tr=sc.animation.tracks[t]; printf("%s{\"name\":",t?",":""); pstr(tr.name); printf(",\"flags\":%d,\"keys\":[",tr.flags);
      for(size_t k=0;k<tr.keys.size();k++){ auto& kf=tr.keys[k]; printf("%s[%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g]",k?",":"",kf.time,kf.trans[0],kf.trans[1],kf.trans[2],kf.rot[0],kf.rot[1],kf.rot[2],kf.rot[3]); }
      printf("]}"); }
    printf("]}"); }
  printf(",\"warnings\":["); for(size_t i=0;i<sc.warnings.size();i++){ printf("%s",i?",":""); pstr(sc.warnings[i]); } printf("]}\n");
  return 0; }
