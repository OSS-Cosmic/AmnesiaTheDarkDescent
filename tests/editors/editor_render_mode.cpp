#include "EditorRenderMode.h"
#include "utest.h"

// The contract under test: the View > Render mode submenu has FOUR items
// because the lit renderer appears once per backend, while a viewport still
// stores only its eRenderer mode. So the menu index is no longer the eRenderer
// value, and every old layout file (RenderMode = 0/1/2) must still land on the
// right item.

using namespace hpl;

UTEST(EditorRenderMode, ShadedItemsDifferOnlyInBackend) {
  const cEditorRenderModeChoice standard =
      EditorRenderModeFromMenuItem(eEditorRenderModeItem_ShadedStandard);
  const cEditorRenderModeChoice rayTraced =
      EditorRenderModeFromMenuItem(eEditorRenderModeItem_ShadedRayTraced);

  ASSERT_EQ(eRenderer_Main, standard.mRenderer);
  ASSERT_EQ(eRenderer_Main, rayTraced.mRenderer);
  ASSERT_TRUE(standard.mbSetsBackend);
  ASSERT_TRUE(rayTraced.mbSetsBackend);
  ASSERT_EQ(eRendererBackend_Standard, standard.mBackend);
  ASSERT_EQ(eRendererBackend_RayTraced, rayTraced.mBackend);
}

UTEST(EditorRenderMode, WireFrameAndSimpleLeaveTheBackendAlone) {
  const cEditorRenderModeChoice wire =
      EditorRenderModeFromMenuItem(eEditorRenderModeItem_WireFrame);
  const cEditorRenderModeChoice simple =
      EditorRenderModeFromMenuItem(eEditorRenderModeItem_Simple);

  ASSERT_EQ(eRenderer_WireFrame, wire.mRenderer);
  ASSERT_EQ(eRenderer_Simple, simple.mRenderer);
  ASSERT_FALSE(wire.mbSetsBackend);
  ASSERT_FALSE(simple.mbSetsBackend);
}

UTEST(EditorRenderMode, OutOfRangeItemFallsBackToShadedStandard) {
  const cEditorRenderModeChoice low = EditorRenderModeFromMenuItem(-1);
  const cEditorRenderModeChoice high =
      EditorRenderModeFromMenuItem(eEditorRenderModeItem_LastEnum);

  ASSERT_EQ(eRenderer_Main, low.mRenderer);
  ASSERT_EQ(eRendererBackend_Standard, low.mBackend);
  ASSERT_EQ(eRenderer_Main, high.mRenderer);
  ASSERT_EQ(eRendererBackend_Standard, high.mBackend);
}

UTEST(EditorRenderMode, ItemRoundTripsUnderItsOwnBackend) {
  for (int i = 0; i < eEditorRenderModeItem_LastEnum; ++i) {
    const cEditorRenderModeChoice choice = EditorRenderModeFromMenuItem(i);
    // Wireframe and Simple carry no backend of their own, so they round-trip
    // under whichever one happens to be active.
    const eRendererBackend active = choice.mbSetsBackend
                                        ? choice.mBackend
                                        : eRendererBackend_Standard;
    ASSERT_EQ(i, EditorRenderModeToMenuItem(choice.mRenderer, active));

    if (choice.mbSetsBackend == false)
      ASSERT_EQ(i, EditorRenderModeToMenuItem(choice.mRenderer,
                                              eRendererBackend_RayTraced));
  }
}

// A layout saved before the split stores the eRenderer int. Loading it must
// check the item matching the ACTIVE backend, never move the viewport's mode.
UTEST(EditorRenderMode, LegacyLayoutValuesMapToTheActiveBackend) {
  ASSERT_EQ(eEditorRenderModeItem_ShadedStandard,
            EditorRenderModeToMenuItem(eRenderer_Main, eRendererBackend_Standard));
  ASSERT_EQ(eEditorRenderModeItem_ShadedRayTraced,
            EditorRenderModeToMenuItem(eRenderer_Main, eRendererBackend_RayTraced));

  ASSERT_EQ(eEditorRenderModeItem_WireFrame,
            EditorRenderModeToMenuItem(eRenderer_WireFrame, eRendererBackend_Standard));
  ASSERT_EQ(eEditorRenderModeItem_WireFrame,
            EditorRenderModeToMenuItem(eRenderer_WireFrame, eRendererBackend_RayTraced));

  ASSERT_EQ(eEditorRenderModeItem_Simple,
            EditorRenderModeToMenuItem(eRenderer_Simple, eRendererBackend_Standard));
  ASSERT_EQ(eEditorRenderModeItem_Simple,
            EditorRenderModeToMenuItem(eRenderer_Simple, eRendererBackend_RayTraced));
}

UTEST_MAIN()
