# Native code looks these up by name through JNI: keep them.
-keepclasseswithmembernames class * { native <methods>; }
-keep class io.github.thekaossphere.calligrus.** { *; }
# GameActivity: native glue calls into the Java classes.
-keep class androidx.games.** { *; }
-keep class com.google.androidgamesdk.** { *; }
