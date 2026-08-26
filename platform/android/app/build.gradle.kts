import com.android.build.api.variant.FilterConfiguration
import com.android.build.gradle.internal.api.ApkVariantOutputImpl
import java.io.FileInputStream
import java.util.Properties
import kotlin.apply

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}
// SET KEY FOR RELEASE BUILD
val localProperties = Properties().apply {
    val localPropertiesFile = rootProject.file("local.properties")
    if (localPropertiesFile.exists()) {
        load(FileInputStream(localPropertiesFile))
    }
}

android {
    signingConfigs {
        create("release") {
            val keyPath = localProperties.getProperty("DEBUG_KEY_PATH") ?: "debug.keystore"
            storeFile = file(keyPath)
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }
    namespace = "com.bassquake.avpvr"
    compileSdk = 36
    ndkVersion = "27.3.13750724"

    defaultConfig {
        // No applicationId here on purpose — every flavor sets its own (see below), so a
        // value at this level would be dead config that looks authoritative.
        minSdk = 24
        targetSdk = 32
        versionCode = 7
        versionName = "0.7"

        externalNativeBuild {
            cmake {
                // STOP REBUILDING compile_commands.json SO VS STOPS RELOADING
                arguments("-DCMAKE_BUILD_TYPE=Release","-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF")
                cppFlags("")
                // Support 16KB compiling
                arguments += listOf("-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON")
                // This ensures the compiler builds for your phone's architecture
                abiFilters.addAll(listOf("arm64-v8a"))
            }
        }
        // MAKE SEPARATE 32bit and 64 BIT APKS.
        //
        // This list is GLOBAL — `splits` is an android {} block, not a defaultConfig
        // one, so Kotlin resolves it to the outer receiver and it applies to every
        // variant. It is also STATIC: AGP emits an APK for each ABI listed here whether
        // or not that ABI has any native libraries in the variant (verified — moving the
        // armeabi-v7a prebuilts out of src/main did not change the output list).
        //
        // externalNativeBuild.abiFilters below does NOT narrow it either; that only
        // decides what CMake compiles. Nor can ndk.abiFilters be used to narrow it —
        // AGP hard-fails with "Conflicting configuration: ... in ndk abiFilters cannot
        // be present when splits abi filters are set".
        //
        // So the quest flavor's unwanted armeabi-v7a APK is switched off by output, in
        // the androidComponents block below.
        splits {
            abi {
                isEnable = true
                reset()
                include("arm64-v8a", "armeabi-v7a")
                isUniversalApk = false
            }
        }
    }
    // IGNORE "Meeting Google Play requirements" because we're using TargetSdk 24
    lint {
        disable += "ExpiredTargetSdkVersion"
    }
    // Two device variants:
    //   quest — VR build. Uses src/main/AndroidManifest.xml (immersive HMD) and the
    //           OpenXR render path. applicationId com.bassquake.quest.avpvr.
    //   android — standard phone/tablet build. Overrides the manifest via src/android (no
    //            VR immersive declarations) and passes -DAVP_DISABLE_XR=ON so OpenXR init
    //            is compiled out and the flat windowed render path is used.
    //            applicationId com.bassquake.android.avpvr.
    //
    // The flavor name is part of every APK filename (avpvr-<ver>-<flavor>-<abi>-<type>.apk)
    // and of the Gradle task names (assembleAndroidRelease / assembleQuestRelease), so it
    // must match the source-set folder under src/.
    //
    // Each flavor sets its own applicationId, so the two install side by side and
    // defaultConfig deliberately sets none. The applicationId is also the external-files
    // path, so assets sideload to
    //   /sdcard/Android/data/<applicationId>/files/
    // — a different directory per flavor.
    //
    // The `namespace` (com.bassquake.avpvr, set at the top) is SHARED and deliberately
    // left alone: it is the code package for R/BuildConfig and for resolving the
    // manifest's relative ".MainActivity" / ".InstallAssets", and both Kotlin sources
    // declare `package com.bassquake.avpvr`. It also cannot be set per flavor —
    // ProductFlavor has no such property, so writing `namespace = ...` inside a flavor
    // block silently resolves against the outer android {} receiver and changes it
    // GLOBALLY (the same implicit-receiver trap as `splits` above), which would break
    // ".MainActivity" resolution in both flavors at launch.
    flavorDimensions += "device"
    productFlavors {
        create("quest") {
            dimension = "device"
            applicationId = "com.bassquake.quest.avpvr"
            // arm64-v8a only — enforced by the androidComponents block below. Every
            // Quest headset is 64-bit ARM, so there is no 32-bit device to serve.
        }
        create("android") {
            dimension = "device"
            applicationId = "com.bassquake.android.avpvr"
            externalNativeBuild {
                cmake {
                    arguments += "-DAVP_DISABLE_XR=ON"
                    // arm64-v8a comes from defaultConfig; add 32-bit ARM for older
                    // phones. Both are packaged as separate per-ABI APKs (splits).
                    abiFilters += "armeabi-v7a"
                }
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    // Assets are NOT bundled — users sideload game files via ADB:
    //   adb push assets/ /sdcard/Android/data/com.bassquake.quest.avpvr/files/   (quest)
    //   adb push assets/ /sdcard/Android/data/com.bassquake.android.avpvr/files/ (android)
    externalNativeBuild {
        cmake {
            // SOURCE PATH
            path = file("${project.rootDir}../../../source/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        viewBinding = true
    }
    /* // HIDE OR INCLUDE ASSETS INTO APK
    androidResources {
        ignoreAssetsPattern = "*"
    }*/
    /* // SET ICONS ETC FOR RELEVANT DEVICES
    florDimensions.add("device")
    productFlavors {
        create("quest") {
            dimension = "device"
        }
        create("android") {
            dimension = "device"
        }
    }*/
    // SET VERSION CODE FOR SEPARATE 32bit/64bit APKS
    android.applicationVariants.all {
        outputs.forEach { output ->
            // Explicitly cast to the Implementation class to access the ABI filter
            val variantOutput = output as? ApkVariantOutputImpl
            val abi = variantOutput?.getFilter(com.android.build.OutputFile.ABI)

            if (abi != null) {
                val abiMultiplier = if (abi == "arm64-v8a") 2 else 1
                val baseVersionCode = versionCode ?: 0
                variantOutput.versionCodeOverride = baseVersionCode * 10 + abiMultiplier
            }
        }
        // COPY FINAL APKS TO build/android FOLDER
        val variant = this
        val variantName = variant.name.capitalize()
        val versionName = variant.versionName

        variant.outputs
            .map { it as com.android.build.gradle.internal.api.BaseVariantOutputImpl }
            .forEach { output ->
                // Use the output name to ensure the task name is unique even if there
                // are multiple APKs.
                val outputName = output.name.capitalize()
                // output.name already carries flavor + ABI + build type
                // ("quest-arm64-v8a-release"), so the two flavors cannot collide in
                // build/android and prefixing variant.flavorName as well would just
                // stutter it: avpvr-quest-0.5-quest-arm64-v8a-release.apk.
                val targetFileName = "avpvr-$versionName-${output.name}.apk"

                // 1. Rename the file in the default build folder
                output.outputFileName = targetFileName

                // 2. Register a UNIQUE task for this specific output
                val copyTask = tasks.register<Copy>("copy${variantName}${outputName}ApkToFolder") {
                    from(output.outputFile.parentFile)
                    into(file("${project.rootDir}/../../build/android"))

                    include(targetFileName)

                    dependsOn(variant.assembleProvider)
                }

                // 3. Automatically run the copy whenever you build this variant
                variant.assembleProvider.configure {
                    finalizedBy(copyTask)
                }

            }
    }
}

// Drop the armeabi-v7a APK from the quest flavor.
//
// splits.abi is global and static, so questRelease/questDebug would otherwise each
// emit a 32-bit APK alongside the 64-bit one. That APK is not merely redundant, it is
// broken: externalNativeBuild.abiFilters builds libavpvr.so for arm64-v8a only, so the
// v7a APK shipped ~28 MB of prebuilt SDL3/FFmpeg/OpenAL/OpenXR .so files with no game
// library at all, and installing it gave an instant crash.
//
// The new Variant API is the only lever that works here — it can switch off a single
// output, which neither ndk.abiFilters (conflicts with splits) nor the source-set
// layout (splits ignore what libs exist) can do. The android flavor is untouched and
// still produces both APKs.
androidComponents {
    onVariants(selector().withFlavor("device" to "quest")) { variant ->
        variant.outputs.forEach { output ->
            val abi = output.filters
                .firstOrNull { it.filterType == FilterConfiguration.FilterType.ABI }
                ?.identifier
            if (abi != null && abi != "arm64-v8a") {
                output.enabled.set(false)
            }
        }
    }
}

    dependencies {

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.constraintlayout)
    //Add Bink and Smacker playback
    //implementation(files("libs/ffmpeg.aar"))
    //implementation("com.arthenica:smart-exception-java:0.2.1")
}