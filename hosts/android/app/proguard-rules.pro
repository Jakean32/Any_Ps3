# Keep JNI bridge entry points (accessed by name from Kotlin)
-keepclasseswithmembernames class com.anyps3.app.** { native <methods>; }
-keep class com.anyps3.app.NativeBridge { *; }
