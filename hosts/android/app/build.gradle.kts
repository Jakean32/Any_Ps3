plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.anyps3.app"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    // Single source of truth for the version string: the VERSION file at the
    // Any_Ps3 repo root (projectDir = hosts/android/app). CMake (native build)
    // and the dev Makefile read the same file, so versionName, the in-app
    // "about" string and the core kEngineVersion always agree.
    val anyps3Version: String = File(projectDir, "../../../VERSION").readText().trim()

    defaultConfig {
        applicationId = "com.anyps3.app"
        minSdk = 28
        targetSdk = 35
        versionCode = 4  // monotonic per-release counter (bumped with each release)
        versionName = anyps3Version
        ndk {
            // Snapdragon first: arm64-v8a only for this deliverable.
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static", "-DANYPS3_BUILD_TESTS=OFF")
                cppFlags += "-std=c++17"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false  // shell build: no point minifying yet
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.compose.ui:ui:1.7.5")
    implementation("androidx.compose.material3:material3:1.3.1")
    implementation("androidx.compose.ui:ui-tooling-preview:1.7.5")
    implementation("androidx.documentfile:documentfile:1.0.1")
}
