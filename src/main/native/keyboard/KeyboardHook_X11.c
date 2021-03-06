#include <X11/Xlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "KeyboardHook.h"

#ifdef DEBUG
#define DEBUG_PRINT(x) puts x
#else
#define DEBUG_PRINT(x) while(0){}
#endif

// declare utility functions
int index_of_listener(JNIEnv*, jobject);
void delete_listener(JNIEnv*, int index);

typedef struct key_listener {
        jobject keyboardHookObject;
        jobject globalKeyListenerObject;
        pthread_t pollingThread;
        volatile int shouldStop;
} KeyListener;

Display *disp;
JavaVM *jvm;
jmethodID processKeyMethod;
KeyListener *listeners;
int listenerCount = 0;

void *poll_key_input_x11(void *arg) {
    int index = *((int*)arg);
    free(arg); // deallocate arg pointer
    KeyListener *kl = &listeners[index];
    struct timespec t1, t2;
    t1.tv_sec = 0;
    t1.tv_nsec = 10000000L; // 10ms sleep timer on polling loop
    JNIEnv *env;
    char prev[32];
    char curr[32];
    while (nanosleep(&t1, &t2) == 0) {
        if (kl->shouldStop) pthread_exit(NULL);

        // attach thread to JVM environment
        if ((*jvm)->AttachCurrentThread(jvm, (void **) &env, NULL) >= 0) {
            XQueryKeymap(disp, curr);
            int i,j;
            for (i = 0; i < 32; i++) {
                char prevKeyByte = prev[i], currKeyByte = curr[i];
                // skip processing for all bits in key byte if nothing changed
                if (currKeyByte == prevKeyByte) continue;
                for (j = 0; j < 8; j++) {
                    int keyCode = i*8 + j;
                    int prevKeyBit = (0x1 << j) & prevKeyByte;
                    int currKeyBit = (0x1 << j) & currKeyByte;
                    if (prevKeyBit == currKeyBit) continue;
                    jboolean transitionState;
                    transitionState = (prevKeyBit > 0) ? JNI_FALSE : JNI_TRUE;
                    (*env)->CallVoidMethod(env, kl->keyboardHookObject, processKeyMethod, transitionState,
                                           (jint) keyCode, kl->globalKeyListenerObject);
                }
                prev[i] = currKeyByte;
            }
            // detach thread from JVM environment
            (*jvm)->DetachCurrentThread(jvm);
        } else {
            fputs("NATIVE: poll_key_input_x11 - Error on attaching current thread.\n", stderr);
            fflush(stderr);
        }
    }
}

JNIEXPORT void JNICALL Java_de_ksquared_system_keyboard_KeyboardHook_registerHook(JNIEnv *env,jobject obj,jobject _globalKeyListenerObject) {
    DEBUG_PRINT("NATIVE: Java_de_ksquared_system_keyboard_KeyboardHook_registerHook - Registering input hook...");
    fflush(stdout);

    const size_t alloc_size = ++listenerCount*sizeof(KeyListener);
    if (listeners == NULL) listeners = (KeyListener*) malloc(alloc_size);
    else listeners = (KeyListener*) realloc(listeners, alloc_size);

    KeyListener kl;

    kl.globalKeyListenerObject = (*env)->NewGlobalRef(env, _globalKeyListenerObject);
    kl.keyboardHookObject = (*env)->NewGlobalRef(env, obj);

    if (!processKeyMethod) {
        jclass cls = (*env)->GetObjectClass(env, kl.keyboardHookObject);
        processKeyMethod = (*env)->GetMethodID(env, cls, "processKey", "(ZILde/ksquared/system/keyboard/GlobalKeyListener;)V");
    }

    if(!disp) disp = XOpenDisplay(0);
    if (!disp) {
        fputs("NATIVE: Java_de_ksquared_system_keyboard_KeyboardHook_registerHook - Failed to open display '0'", stderr);
        fflush(stderr);
        return;
    }

    (*env)->GetJavaVM(env, &jvm);

    listeners[listenerCount - 1] = kl;

    pthread_attr_t thread_config;
    pthread_attr_init(&thread_config);
    int *arg = malloc(sizeof(int));
    *arg = listenerCount - 1;
    // start thread with thread_config and the current listener index as the argument
    int err = pthread_create(&kl.pollingThread, &thread_config, poll_key_input_x11, (void*) arg);

    if (err) {
        fputs("NATIVE: Java_de_ksquared_system_keyboard_KeyboardHook_registerHook - Failed to launch new pthread.", stderr);
        fflush(stderr);
        delete_listener(env, listenerCount - 1);
        return;
    }

    DEBUG_PRINT("NATIVE: Java_de_ksquared_system_keyboard_KeyboardHook_registerHook - Successfully initialized event polling!");
    fflush(stdout);
}

JNIEXPORT void JNICALL Java_de_ksquared_system_keyboard_KeyboardHook_unregisterHook(JNIEnv *env, jobject object) {
    int index = index_of_listener(env, object);
    if (index < 0) {
        DEBUG_PRINT("NATIVE: Java_de_ksquared_system_keyboard_KeyboardHook_unregisterHook - No matching listener registered!");
        return;
    }
    KeyListener *kl = &listeners[index];
    pthread_t thread = kl->pollingThread;
    // notify thread to terminate
    kl->shouldStop = 1;
    pthread_join(thread, NULL);
    delete_listener(env, index);
    if (listenerCount == 0) {
        XCloseDisplay(disp);
        disp = NULL;
    }

    DEBUG_PRINT("NATIVE: Java_de_ksquared_system_keyboard_KeyboardHook_unregisterHook - Sucessfully unregistered hook.");
    pthread_exit(NULL);
}

// utility function implementations

int index_of_listener(JNIEnv *env, jobject keyboardHookObject) {
    int i, index = -1;
    for (i = 0; i < listenerCount; i++) {
        KeyListener *next = &listeners[i];
        if ((*env)->IsSameObject(env, keyboardHookObject, next->keyboardHookObject)) {
            index = i;
            break;
        }
    }
    return index;
}

void delete_listener(JNIEnv *env, int index) {
    if (index < 0) return;
    KeyListener *kl = &listeners[index];
    // delete jni resources and detach thread
    (*env)->DeleteGlobalRef(env, kl->keyboardHookObject);
    (*env)->DeleteGlobalRef(env, kl->globalKeyListenerObject);

    // remove from listener array
    KeyListener *lreduced = malloc(--listenerCount * sizeof(KeyListener));
    int offs0 = index * sizeof(KeyListener);
    int offs1 = offs0 + sizeof(KeyListener);
    memcpy(lreduced, listeners, index * sizeof(KeyListener));
    memcpy(lreduced + offs0, listeners + offs1, (listenerCount - index) * sizeof(KeyListener));
    free(listeners);
    listeners = lreduced;
}
