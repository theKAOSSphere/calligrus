# The Android shell (included from CMakeLists.txt when building for Android; the Gradle project is android/).
#
# `calligrus` is a shared library that GameActivity loads (libcalligrus.so). It is the desktop app's own
# sources (main.cpp and everything else) plus this folder's window layer:
#   src/android/android_main.cpp     the entry point (GameActivity's android_main)
#   src/android/window_android.*     the part of GLFW main.cpp uses, over EGL / OpenGL ES 3, plus touch
#   src/android/platform_android.cpp platform.h for Android, JNI calls into MainActivity.java
# (platform_android.cpp is already in CALLIGRUS_PLATFORM_SRC.)

# GameActivity's native glue and headers: a prefab package from the games-activity AAR (Gradle).
find_package(game-activity REQUIRED CONFIG)

target_sources(calligrus PRIVATE src/android/android_main.cpp src/android/window_android.cpp)
target_link_libraries(calligrus PRIVATE game-activity::game-activity_static android EGL GLESv3 log)

# GameActivity's Java side looks this function up in our library; keep the linker from dropping it.
target_link_options(calligrus PRIVATE "SHELL:-u Java_com_google_androidgamesdk_GameActivity_initializeNativeCode")
