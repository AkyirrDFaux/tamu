plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    // Do not apply the Kotlin Gradle Plugin (id("kotlin-android")) by hand: Flutter applies it
    // for the app module when the app leaves it out, and applying it ourselves breaks in a
    // future Flutter.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "tamu.app.tamuapp"
    // permission_handler_android 14.1.0 compiles against Android SDK 37, so the app compiles
    // against it too (a higher compileSdk is backward compatible). Flutter's default
    // (flutter.compileSdkVersion) is still 36, and the Flutter Gradle plugin warns while the
    // app sits below a plugin's compileSdk.
    compileSdk = 37
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "tamu.app"
        // Beta builds install ALONGSIDE the previous release instead of updating
        // it (Android identifies apps by applicationId, not by the label). Remove
        // this suffix once the beta is promoted to the main app.
        applicationIdSuffix = ".beta"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName
        // Launcher name (see AndroidManifest's android:label).
        manifestPlaceholders["appName"] = "Tamu App (beta)"
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

// Replaces the old `kotlinOptions { jvmTarget = ... }` block, which only exists while the
// Kotlin Gradle Plugin is applied to this module (see Flutter's built-in-Kotlin migration).
kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
