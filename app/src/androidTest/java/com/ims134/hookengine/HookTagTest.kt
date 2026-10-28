package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 71 — Feature 6.2: per-guest tags.
 *
 * Verifies the six assertions spec'd in PROJECT_STATE §6.2 plus one extra
 * (`tagPendingClearsOnSuccessAndFailure`) that pins the thread-local
 * pending-tag RAII contract.
 *
 * ─── Design constraint being tested ────────────────────────────────
 *
 * §6.2: "tags partition uninstall scope only, never install admissibility."
 * Concretely: FindHandle(void* target) must stay global — one address holds
 * one patch, so a second install on an already-owned address returns
 * HOOK_ALREADY_HOOKED (or HOOK_INTERNAL_ERROR for a different replacement)
 * regardless of tag.
 *
 * `conflictDetectionIgnoresTag` uses installTagFixtureAlt to force the
 * second install past HookGotEntryEx's early `*gotAddress == newFunction`
 * return, exercising FindHandle directly. A tag-aware FindHandle would
 * return HOOK_OK (silently double-patch the slot); the test then fails on
 * `HOOK_INTERNAL_ERROR` expected vs HOOK_OK actual.
 *
 * ─── Fixture layout ────────────────────────────────────────────────
 *
 * 5 slots. Each has a real fn (returns 10+i + x), a primary replacement
 * (increments g_tagReplCalls[i], returns 1000+x), an alt replacement
 * (increments g_tagAltCalls[i], returns 3000+x), and a GOT slot variable
 * whose current value points at whichever function is installed.
 *
 * @Before/@After call uninstallAll + resetTagFixtures so no state leaks
 * between tests, even on assertion failure.
 */
@RunWith(AndroidJUnit4::class)
class HookTagTest {

    private val TAG = "HookTagTest"

    // HookResult codes from HookRegistry.h
    private val HOOK_OK              = 0
    private val HOOK_ALREADY_HOOKED  = 8
    private val HOOK_INTERNAL_ERROR  = 10

    @Before
    fun setup() {
        NativeTestBridge.uninstallAll()
        NativeTestBridge.resetTagFixtures()
    }

    @After
    fun teardown() {
        NativeTestBridge.uninstallAll()
        NativeTestBridge.resetTagFixtures()
    }

    // ─── 1. tagIsEmptyByDefault ───────────────────────────────────────
    @Test
    fun tagIsEmptyByDefault() {
        val rc = NativeTestBridge.installTagFixture(0, null)
        Log.d(TAG, "installTagFixture(0, null) = $rc")
        assertEquals("install with null tag should succeed", HOOK_OK, rc)
        assertEquals("no handles tagged 'myscope'",
            0, NativeTestBridge.registryCountByTag("myscope"))
        assertTrue("at least one untagged handle exists",
            NativeTestBridge.registryCountByTag("") >= 1)
        val snap = NativeTestBridge.registrySnapshotByTag("", 16)
        assertNotNull("untagged snapshot must be non-null", snap)
        assertTrue("untagged snapshot must be non-empty", snap!!.isNotEmpty())
    }

    // ─── 2. tagSetAtInstallIsVisible ──────────────────────────────────
    @Test
    fun tagSetAtInstallIsVisible() {
        val rc = NativeTestBridge.installTagFixture(0, "myscope")
        assertEquals("install with tag 'myscope' should succeed", HOOK_OK, rc)
        assertEquals("one handle tagged 'myscope'",
            1, NativeTestBridge.registryCountByTag("myscope"))
        assertEquals("no handles tagged 'other'",
            0, NativeTestBridge.registryCountByTag("other"))
        val tags = NativeTestBridge.registryListTags()
        Log.d(TAG, "tags after install: ${tags.replace("\n", "|")}")
        assertTrue("'myscope' appears as a whole line in the tag list",
            tags.lines().any { it == "myscope" })
    }

    // ─── 3. uninstallByTagRemovesOnlyThatSubset ───────────────────────
    @Test
    fun uninstallByTagRemovesOnlyThatSubset() {
        for (i in 0..1) assertEquals("install A#$i", HOOK_OK,
            NativeTestBridge.installTagFixture(i, "A"))
        for (i in 2..4) assertEquals("install B#$i", HOOK_OK,
            NativeTestBridge.installTagFixture(i, "B"))
        assertEquals(2, NativeTestBridge.registryCountByTag("A"))
        assertEquals(3, NativeTestBridge.registryCountByTag("B"))

        // Sanity: a B fixture fires its replacement before uninstall.
        val beforeB2 = NativeTestBridge.getTagReplCalls(2)
        NativeTestBridge.callTagFixture(2, 5)
        assertEquals("B fixture #2 fires before uninstall",
            beforeB2 + 1, NativeTestBridge.getTagReplCalls(2))

        // Uninstall "A".
        val result = NativeTestBridge.registryUninstallByTag("A")
        assertNotNull(result)
        assertEquals(2, result!!.size)
        assertEquals("removed 2 A-hooks", 2, result[0])
        assertEquals("no uninstall failures", 0, result[1])
        assertEquals("A count now 0", 0,
            NativeTestBridge.registryCountByTag("A"))
        assertEquals("B count unchanged at 3", 3,
            NativeTestBridge.registryCountByTag("B"))

        // B fixture #3 still fires its replacement.
        val beforeB3 = NativeTestBridge.getTagReplCalls(3)
        NativeTestBridge.callTagFixture(3, 7)
        assertEquals("B fixture #3 fires after A uninstall",
            beforeB3 + 1, NativeTestBridge.getTagReplCalls(3))

        // A fixture #0 no longer fires its replacement (bytes restored).
        val beforeA0 = NativeTestBridge.getTagReplCalls(0)
        NativeTestBridge.callTagFixture(0, 3)
        assertEquals("A fixture #0 no longer fires after uninstall",
            beforeA0, NativeTestBridge.getTagReplCalls(0))
    }

    // ─── 4. uninstallByTagIsIdempotentAndReportsZero ─────────────────
    @Test
    fun uninstallByTagIsIdempotentAndReportsZero() {
        assertEquals(HOOK_OK, NativeTestBridge.installTagFixture(0, "X"))
        assertEquals(1, NativeTestBridge.registryCountByTag("X"))

        val first = NativeTestBridge.registryUninstallByTag("X")
        assertNotNull(first)
        assertEquals("first uninstall removes 1", 1, first!![0])
        assertEquals("first uninstall reports no failures", 0, first[1])

        val second = NativeTestBridge.registryUninstallByTag("X")
        assertNotNull(second)
        assertEquals("second uninstall removes 0", 0, second!![0])
        assertEquals("second uninstall reports no failures", 0, second[1])
    }

    // ─── 5. conflictDetectionIgnoresTag ──────────────────────────────
    //
    // Two installs on the same slot: tag "A" with primary replacement,
    // then tag "B" with the ALT replacement. Because the alt replacement
    // is a different function pointer from the primary, the second
    // install is forced past `HookGotEntryEx`'s early
    // `*gotAddress == newFunction` return and into `FindHandle`. FindHandle
    // finds the existing handle (tag "A", replacement=g_tagRepls[0]) with
    // a different replacement (g_tagAlts[0]) and returns
    // HOOK_INTERNAL_ERROR. A tag-aware FindHandle would return HOOK_OK.
    @Test
    fun conflictDetectionIgnoresTag() {
        assertEquals(HOOK_OK, NativeTestBridge.installTagFixture(0, "A"))
        val rc = NativeTestBridge.installTagFixtureAlt(0, "B")
        Log.d(TAG, "installTagFixtureAlt(0, B) after A = $rc")
        assertEquals("FindHandle must refuse reinstall regardless of tag",
            HOOK_INTERNAL_ERROR, rc)
        assertEquals("original tag A count unchanged",
            1, NativeTestBridge.registryCountByTag("A"))
        assertEquals("no handle acquired tag B",
            0, NativeTestBridge.registryCountByTag("B"))

        // Slot 0 is still patched with the primary replacement — the
        // alternate never took effect.
        NativeTestBridge.callTagFixture(0, 5)
        assertTrue("primary replacement still active",
            NativeTestBridge.getTagReplCalls(0) >= 1)
        assertEquals("alternate replacement never ran", 0,
            NativeTestBridge.getTagAltCalls(0))
    }

    // ─── 6. tagSurvivesSnapshotCopy ──────────────────────────────────
    @Test
    fun tagSurvivesSnapshotCopy() {
        assertEquals(HOOK_OK, NativeTestBridge.installTagFixture(0, "preserved"))
        val snap = NativeTestBridge.registrySnapshotByTag("preserved", 16)
        assertNotNull(snap)
        assertEquals(1, snap!!.size)

        // Mutate the returned array — must not affect the registry.
        snap[0] = 0L
        assertEquals("snapshot is a copy, not a live view",
            1, NativeTestBridge.registryCountByTag("preserved"))
    }

    // ─── 7. tagPendingClearsOnSuccessAndFailure (extra vs §6.2) ──────
    @Test
    fun tagPendingClearsOnSuccessAndFailure() {
        // Success path: install with "A", then install another fixture
        // with NO tag. The second must be untagged.
        assertEquals(HOOK_OK, NativeTestBridge.installTagFixture(0, "A"))
        assertEquals(HOOK_OK, NativeTestBridge.installTagFixture(1, null))
        assertEquals(1, NativeTestBridge.registryCountByTag("A"))
        assertTrue("fixture #1 must be untagged",
            NativeTestBridge.registryCountByTag("") >= 1)

        // Failure path: reinstall slot 0 with tag "B" (same replacement,
        // early-return). The pending tag must be cleared even though the
        // install failed.
        val rc = NativeTestBridge.installTagFixture(0, "B")
        assertEquals("second install on slot 0 returns HOOK_ALREADY_HOOKED",
            HOOK_ALREADY_HOOKED, rc)
        assertEquals("no handle acquired tag B", 0,
            NativeTestBridge.registryCountByTag("B"))

        // Install slot 2 with NO tag. If the pending tag leaked from the
        // failed install, slot 2 would inherit "B" — this assertion pins
        // the cleanup contract.
        assertEquals(HOOK_OK, NativeTestBridge.installTagFixture(2, null))
        assertEquals("B did not leak to slot 2", 0,
            NativeTestBridge.registryCountByTag("B"))
    }
}