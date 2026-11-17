plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.sdrstudio"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.sdrstudio"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        // Les options de compilation (-std=c++20, -O3, -ffast-math) sont dans
        // CMakeLists.txt : une seule source de verite.
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
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
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // appcompat suffit : aucune extension core-ktx n'est utilisee.
    implementation("androidx.appcompat:appcompat:1.7.0")
}
