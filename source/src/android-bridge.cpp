#include <jni.h>
#include <string>

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_freesynd_MainActivity_stringFromCpp(
        JNIEnv* env,
        jobject /* this */) {

    std::string hello = "AvPVR Engine Initialized";
    return env->NewStringUTF(hello.c_str());
}