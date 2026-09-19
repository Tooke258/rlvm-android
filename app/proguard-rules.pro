# JNI 符号由 JNI_OnLoad + RegisterNatives 显式注册，方法名不参与反射查找，
# 但仍需保证 NativeBridge 类名与 native 侧常量一致，故整体保留。
-keep class org.rlvm.android.NativeBridge { *; }
