# Tactical Implementation Plan: MainActivity Jetpack Compose Migration

## Roadmap
This implementation plan breaks down the migration of `MainActivity` and `activity_main.xml` to Jetpack Compose into sequential, verifiable tasks using the Direct Replacement strategy.

---

## Task Checklist

### Task_1_ComposeSetupAndTheme
- **Description:** Enable Jetpack Compose in `app/build.gradle.kts` (or `build.gradle`), add Compose BOM, Compose UI, Material3, and Preview dependencies, and create the basic Material3 Theme.
- **Files:**
  - `app/build.gradle` (modify)
  - `app/src/main/java/com/ims134/hookengine/ui/theme/Theme.kt` (create)
  - `app/src/main/java/com/ims134/hookengine/ui/theme/Color.kt` (create)
  - `app/src/main/java/com/ims134/hookengine/ui/theme/Type.kt` (create)
- **Depends on:** `None`
- **Interfaces:** `HookEngineTheme` composable wrapper.
- **Acceptance Criteria:** Project syncs successfully with Compose compiler and dependencies enabled.

### Task_2_CreateMainScreenComposable
- **Description:** Implement the `MainScreen` composable matching the exact centered layout of `activity_main.xml` with a Compose Preview.
- **Files:**
  - `app/src/main/java/com/ims134/hookengine/ui/MainScreen.kt` (create)
- **Depends on:** `Task_1_ComposeSetupAndTheme`
- **Interfaces:** `MainScreen(text: String, modifier: Modifier)`
- **Acceptance Criteria:** `MainScreen` renders correctly in Compose Preview with centered text.

### Task_3_RefactorMainActivity
- **Description:** Update `MainActivity.kt` to use `setContent` with `MainScreen` and the native `stringFromJNI()` call, removing View Binding references.
- **Files:**
  - `app/src/main/java/com/ims134/hookengine/MainActivity.kt` (modify)
- **Depends on:** `Task_2_CreateMainScreenComposable`
- **Interfaces:** `MainActivity` launching `MainScreen`.
- **Acceptance Criteria:** `MainActivity` compiles and runs successfully, displaying the JNI string via Jetpack Compose.

### Task_4_CleanupLegacyXmlAndVerify
- **Description:** Remove the legacy `activity_main.xml` layout file and run Gradle build and tests to verify clean compilation.
- **Files:**
  - `app/src/main/res/layout/activity_main.xml` (delete)
- **Depends on:** `Task_3_RefactorMainActivity`
- **Interfaces:** None
- **Acceptance Criteria:** Build succeeds with zero unresolved references or missing layout files.
