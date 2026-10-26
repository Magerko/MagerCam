import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.magercam.app"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.magercam.app"
        minSdk = 26
        targetSdk = 34
        versionCode = 4
        versionName = "1.1.3"
        // x86_64 только для эмулятора; 32-битный x86 давно не встречается
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    }

    androidResources {
        // модель читаю через mmap, а это работает только с несжатым файлом
        noCompress += "tflite"
    }

    // свой ключ лежит вне репозитория и описан в android/keystore.properties; без него подписываю отладочным
    val keyProps = rootProject.file("keystore.properties").takeIf { it.exists() }
        ?.let { f -> Properties().apply { f.inputStream().use { load(it) } } }

    signingConfigs {
        if (keyProps != null) create("release") {
            storeFile = file(keyProps.getProperty("storeFile"))
            storePassword = keyProps.getProperty("storePassword")
            keyAlias = keyProps.getProperty("keyAlias")
            keyPassword = keyProps.getProperty("keyPassword")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (keyProps != null) "release" else "debug")
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
    // распознавание QR при сопряжении с ПК, чистая Java без Google Play Services
    implementation("com.google.zxing:core:3.5.3")
    // сегментация человека для размытия и замены фона (модель MediaPipe selfie_segmenter в assets, работает без интернета)
    implementation("org.tensorflow:tensorflow-lite:2.16.1")
}
