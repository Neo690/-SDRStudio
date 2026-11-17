// Stub minimal de <jni.h>.
//
// OBJECTIF UNIQUE : permettre de verifier, avec un compilateur de bureau, que
// le pont JNI (android/app/src/main/cpp/sdr_jni.cpp) compile toujours contre
// l'API reelle du coeur DSP (core/dsp.h, core/source.h).
//
// Ce fichier N'EST PAS le vrai en-tete JNI et ne produit aucune bibliotheque
// Android utilisable : il ne fait que declarer les types et methodes employes
// par le pont. Le vrai <jni.h> vient du NDK Android.
#pragma once

#include <cstdint>

typedef uint8_t jboolean;
typedef int32_t jint;
typedef int32_t jsize;
typedef float jfloat;
typedef double jdouble;

struct _jobject;
typedef _jobject* jobject;
typedef _jobject* jclass;
typedef _jobject* jarray;
typedef _jobject* jstring;
typedef jarray jfloatArray;
typedef jarray jdoubleArray;

#define JNIEXPORT
#define JNICALL
#define JNI_TRUE 1
#define JNI_FALSE 0

// Sous-ensemble de JNIEnv reellement utilise par sdr_jni.cpp.
struct JNIEnv {
    jsize GetArrayLength(jarray array);
    void SetFloatArrayRegion(jfloatArray array, jsize start, jsize len, const jfloat* buf);
    jfloat* GetFloatArrayElements(jfloatArray array, jboolean* isCopy);
    void ReleaseFloatArrayElements(jfloatArray array, jfloat* elems, jint mode);
    jfloatArray NewFloatArray(jsize length);
    const char* GetStringUTFChars(jstring str, jboolean* isCopy);
    void ReleaseStringUTFChars(jstring str, const char* chars);
    jstring NewStringUTF(const char* utf);
};

typedef jint (JNICALL* JNI_OnLoad_t)(void* vm, void* reserved);
