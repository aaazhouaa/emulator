// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Top-level build file where you can add configuration options common to all subprojects/modules.
plugins {
    id("com.android.application") version "9.4.1" apply false
    id("com.android.library") version "9.4.1" apply false
    id("org.jetbrains.kotlin.android") version "2.4.20" apply false
}

tasks.register<Delete>("clean") {
    description = "Cleans project build outputs"
    delete(layout.buildDirectory)
}

// Fail packaging of the release variant when no distribution keystore was supplied.
// Leaving `release.signingConfig` unset makes AGP emit `-unsigned.apk` and still report
// BUILD SUCCESSFUL, which is just as easy to ship by accident as the debug-key fallback
// this replaced.
allprojects {
    tasks.matching { it.name == "packageMainlineRelease" || it.name == "packageEaRelease" }
        .configureEach {
            doFirst {
                check(System.getenv("ANDROID_KEYSTORE_FILE") != null) {
                    "refusing to package a release APK without signing credentials: " +
                        "ANDROID_KEYSTORE_FILE is not set. Source the signing environment " +
                        "before invoking Gradle (the variables must be exported)."
                }
            }
        }
}

buildscript {
    repositories {
        google()
    }
    dependencies {
        classpath("androidx.navigation:navigation-safe-args-gradle-plugin:2.10.0")
    }
}
