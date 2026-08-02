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
        applicationId = "com.bassquake.avpvr"
        minSdk = 24
        targetSdk = 32
        versionCode = 5
        versionName = "0.5"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

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
        // MAKE SEPARATE 32bit and 64 BIT APKS. Both ABIs are listed here, but each
        // flavor's externalNativeBuild.abiFilters decides which it actually builds:
        // quest = arm64-v8a only (from defaultConfig); mobile also builds armeabi-v7a.
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
    //           OpenXR render path. Unchanged from before flavors existed.
    //   mobile — standard phone/tablet build. Overrides the manifest via src/mobile (no
    //            VR immersive declarations) and passes -DAVP_DISABLE_XR=ON so OpenXR init
    //            is compiled out and the flat windowed render path is used. Its own
    //            applicationId (com.bassquake.mobile.avpvr) so it can't clash with the
    //            Quest package. (namespace stays com.bassquake.avpvr — that's the code
    //            package for R/BuildConfig and the .MainActivity class, independent of
    //            the installed applicationId.)
    flavorDimensions += "device"
    productFlavors {
        create("quest") {
            dimension = "device"
        }
        create("mobile") {
            dimension = "device"
            applicationId = "com.bassquake.mobile.avpvr"
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
    //   adb push assets/ /sdcard/Android/data/com.bassquake.avpvr/files/
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
        create("mobile") {
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
                // Use the output name (e.g., "arm64-v8a", "x86", or "universal")
                // to ensure the task name is unique even if there are multiple APKs
                val outputName = output.name.capitalize()
                // Include the flavor (quest/phone) so the two variants' APKs don't
                // collide on the same filename in build/android.
                val targetFileName = "avpvr-${variant.flavorName}-$versionName-${output.name}.apk"

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

    dependencies {

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.constraintlayout)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    //Add Bink and Smacker playback
    //implementation(files("libs/ffmpeg.aar"))
    //implementation("com.arthenica:smart-exception-java:0.2.1")
}