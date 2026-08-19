# ARIFT ProGuard rules — keep native bridge API and service entry points intact.
-keep class com.arift.injector.core.NativeBridge { *; }
-keep class com.arift.injector.ui.CheatOverlayService { *; }
-keep class com.arift.injector.vae.VaeSpawner { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.arift.injector.** { *; }
-dontwarn org.slf4j.**
-dontwarn org.jetbrains.annotations.**