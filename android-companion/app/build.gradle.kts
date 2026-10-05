plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }
android {
    namespace="dev.minios.companion"
    compileSdk=36
    ndkVersion="27.2.12479018"
    defaultConfig {
        applicationId="dev.minios.companion";minSdk=26;targetSdk=36
        versionCode=1;versionName="1.0"
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
        externalNativeBuild { cmake { arguments += "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON" } }
    }
    externalNativeBuild { cmake { path=file("src/main/cpp/CMakeLists.txt");version="3.22.1" } }
    compileOptions { sourceCompatibility=JavaVersion.VERSION_17;targetCompatibility=JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget="17" }
}
dependencies { testImplementation("junit:junit:4.13.2") }
