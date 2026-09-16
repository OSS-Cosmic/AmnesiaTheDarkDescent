// Link stubs for the engine math this suite pulls in. cMath, cFrustum and
// cBoundingVolume reference a handful of symbols from translation units that
// would otherwise drag SDL, tinyxml2 and the vertex buffer implementation into
// a pure math test. None of them is reachable from the code under test.
#include "graphics/Color.h"
#include "graphics/VertexBuffer.h"
#include "system/SerializeClass.h"
#include "system/String.h"

namespace hpl {
void Error(const char *, ...) {}

cColor::cColor() : r(0), g(0), b(0), a(0) {}
cColor::cColor(float afR, float afG, float afB, float afA)
    : r(afR), g(afG), b(afB), a(afA) {}

cSerializeClass::cSerializeClass(const char *, const char *,
                                 cSerializeMemberField *, size_t,
                                 iSerializable *(*)()) {}

tString cString::Sub(const tString &asString, int alStart, int alCount) {
  return alCount < 0 ? asString.substr(alStart)
                     : asString.substr(alStart, alCount);
}
tWString cString::SubW(const tWString &asString, int alStart, int alCount) {
  return alCount < 0 ? asString.substr(alStart)
                     : asString.substr(alStart, alCount);
}

float *cVertexBuffer::GetFloatArray(eVertexBufferElement) { return nullptr; }
int cVertexBuffer::GetElementNum(eVertexBufferElement) { return 0; }
int cVertexBuffer::GetIndexNum() { return 0; }
unsigned int *cVertexBuffer::GetIndices() { return nullptr; }
} // namespace hpl
