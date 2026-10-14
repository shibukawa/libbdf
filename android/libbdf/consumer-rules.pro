# The JNI layer finds these by their names.
-keepclasseswithmembernames class io.github.shibukawa.bdf.Native {
    native <methods>;
}
-keep class io.github.shibukawa.bdf.BdfException {
    <init>(int, java.lang.String);
}
