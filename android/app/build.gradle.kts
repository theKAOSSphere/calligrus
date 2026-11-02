plugins {
    id("com.android.application")
}

// The native code is the repository's own CMakeLists.txt (two folders above this one); its third-party libraries
// are downloaded and built by CMake itself for each ABI (FetchContent, cmake/deps.cmake), see ../../BUILDING.md.
// The downloaded archives are kept in $CALLIGRUS_DOWNLOAD_DIR when that is set (CI caches it).

// Which CPUs to build: arm64-v8a is the tablet, x86_64 the emulator.
// Override: ./gradlew assembleDebug -PcalligrusAbis=arm64-v8a
val nativeDebug: Boolean = (project.findProperty("calligrusNativeDebug") as String?) == "true"

// Release builds default to arm64-v8a only (real devices); debug builds default to both.
// -PcalligrusAbis=arm64-v8a,x86_64 overrides either.
val releaseRequested: Boolean = gradle.startParameter.taskNames.any { it.contains("release", ignoreCase = true) }

val abis: List<String> =
    ((project.findProperty("calligrusAbis") as String?)
        ?: if (releaseRequested) "arm64-v8a" else "arm64-v8a,x86_64").split(",").map { it.trim() }

// The one place the version lives is the project() line of the repository's CMakeLists.txt.
val cmakeVersion: String =
    Regex("""project\(\s*calligrus\s+VERSION\s+([0-9][0-9.]*)""")
        .find(file("../../CMakeLists.txt").readText())?.groupValues?.get(1) ?: "0.0.0"

// Optional release signing, for CI: only when all required environment variables are set. Nothing is
// ever read from a file in this tree. Without them the release APK is left unsigned.
val relKeystore: String? = System.getenv("CALLIGRUS_RELEASE_KEYSTORE")?.takeIf { it.isNotBlank() }
val relStorePass: String? = System.getenv("CALLIGRUS_RELEASE_STORE_PASSWORD")?.takeIf { it.isNotEmpty() }
val relAlias: String = System.getenv("CALLIGRUS_RELEASE_KEY_ALIAS")?.takeIf { it.isNotBlank() } ?: "calligrus"
val relKeyPass: String? = System.getenv("CALLIGRUS_RELEASE_KEY_PASSWORD")?.takeIf { it.isNotEmpty() } ?: relStorePass
val signRelease: Boolean = relKeystore != null && relStorePass != null

android {
    namespace = "io.github.thekaossphere.calligrus"
    compileSdk = 36
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "io.github.thekaossphere.calligrus"
        minSdk = 26
        targetSdk = 36
        versionCode = 3
        versionName = cmakeVersion

        ndk {
            abiFilters += abis
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    // One static C++ runtime inside libcalligrus.so, shared by the libraries built with it.
                    "-DANDROID_STL=c++_static",
                    // GameActivity's native glue wants RTTI and exceptions.
                    "-DANDROID_CPP_FEATURES=rtti exceptions",
                    // Android 15+ devices may use 16 KB memory pages.
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    // The debug APK (the Java side stays debuggable) carries optimised native code without debug
                    // information: -PcalligrusNativeDebug=true gives -O0 -g for a native debugger.
                    "-DCMAKE_BUILD_TYPE=${if (nativeDebug) "Debug" else "Release"}"
                )
                cppFlags += "-std=c++20"
            }
        }
    }

    signingConfigs {
        if (signRelease) {
            create("release") {
                storeFile = file(relKeystore!!)
                storePassword = relStorePass
                keyAlias = relAlias
                keyPassword = relKeyPass
            }
        }
    }

    buildTypes {
        release {
            // R8 (see proguard-rules.pro). Native code is already Release and stripped (packaging below).
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (signRelease) signingConfig = signingConfigs.getByName("release")
        }
    }

    buildFeatures {
        // GameActivity's C++ headers and static library come from the AAR through "prefab", so CMake can
        // find_package() them.
        prefab = true
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs {
            // The libraries stay compressed inside the APK (and are loaded from it): a much smaller file.
            useLegacyPackaging = true
            // Strip what debug information is left (when native debugging is off).
            keepDebugSymbols.clear()
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    // Makes all Kotlin standard-library pieces the same version; without it the build fails with
    // "Duplicate class kotlin.*" (appcompat pulls two versions).
    implementation(platform("org.jetbrains.kotlin:kotlin-bom:1.8.22"))

    // GameActivity: the Java activity plus the native glue (android_main).
    implementation("androidx.games:games-activity:3.0.5")

    // GameActivity extends AppCompatActivity but does not declare the dependency, so without this the app
    // crashes at launch with ClassNotFoundException. It also requires an AppCompat theme (see themes.xml).
    implementation("androidx.appcompat:appcompat:1.7.0")
}
