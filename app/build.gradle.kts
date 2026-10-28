plugins {
    alias(libs.plugins.android.application)
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.ims134.hookengine"
    compileSdk = 37 // Fixed syntax for compileSdk
    ndkVersion = "30.0.16248370"
    testBuildType ="debug"

    defaultConfig {
        applicationId = "com.ims134.hookengine"
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false // Set to true if you want code shrinking
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    // S68 note: the `packaging { jniLibs { keepDebugSymbols += "**/..." } }`
    // block was tried here to prevent AGP from stripping .symtab from
    // libhookengine.so. On this toolchain (AGP + NDK 30.0.16248370) it
    // does not affect CMake-built libraries — .symtab is removed during
    // the APK packaging step regardless. Removed here so it doesn't
    // mislead future readers. Feature 7.1's .symtab-fallback test uses
    // libc.so (which ships with .symtab on this device) instead.

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        viewBinding = true
        compose = true
    }

    // REMOVED: composeOptions { kotlinCompilerExtensionVersion = "1.5.1" }
    // (This is handled automatically by the kotlin.plugin.compose plugin)
}

dependencies {
    implementation(libs.androidx.junit.ktx)
    // Compose BOM manages all compose versions perfectly
    val composeBom = platform("androidx.compose:compose-bom:2024.02.00")
    implementation(composeBom)

    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.ui)
    implementation(libs.androidx.material3)

    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.core.ktx)
    implementation(libs.material)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}