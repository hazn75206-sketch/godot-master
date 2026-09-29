#include "mcp_android.h"

#include "mcp_server.h"

#ifdef ANDROID_ENABLED
#include "platform/android/jni_utils.h"
#include "platform/android/java_godot_wrapper.h"
#include "platform/android/os_android.h"
#endif

#if defined(ANDROID_ENABLED)

static bool mcp_jni_ready = false;

// Called from Java (McpServerService) when the user presses "Matikan server".
extern "C" JNIEXPORT void JNICALL Java_org_godotengine_godot_mcp_McpServerService_notifyMcpStop(JNIEnv *env, jclass clazz) {
	McpServer *s = McpServer::get_singleton();
	if (s) {
		s->set_enabled(false);
	}
}

// Called from Java (McpServerService) to get the current MCP URL to copy.
extern "C" JNIEXPORT jstring JNICALL Java_org_godotengine_godot_mcp_McpServerService_getServerUrl(JNIEnv *env, jclass clazz) {
	McpServer *s = McpServer::get_singleton();
	String url = s ? s->get_mcp_url() : String();
	return env->NewStringUTF(url.utf8().get_data());
}

static jobject mcp_get_activity(JNIEnv *env) {
	OS_Android *os = OS_Android::get_singleton();
	if (!os || !os->get_godot_java()) {
		return nullptr;
	}
	return os->get_godot_java()->get_activity();
}

void mcp_android_notification_on() {
	JNIEnv *env = get_jni_env();
	if (!env) {
		return;
	}
	jclass cls = jni_find_class(env, "org/godotengine/godot/mcp/McpServerService");
	if (!cls) {
		return;
	}
	jmethodID start = env->GetStaticMethodID(cls, "start", "(Landroid/content/Context;I)V");
	if (start) {
		jobject activity = mcp_get_activity(env);
		if (activity) {
			McpServer *s = McpServer::get_singleton();
			jint port = s ? (jint)s->get_port() : 8766;
			env->CallStaticVoidMethod(cls, start, activity, port);
			mcp_jni_ready = true;
		}
	}
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
	}
	env->DeleteLocalRef(cls);
}

void mcp_android_notification_off() {
	JNIEnv *env = get_jni_env();
	if (!env) {
		return;
	}
	jclass cls = jni_find_class(env, "org/godotengine/godot/mcp/McpServerService");
	if (!cls) {
		return;
	}
	jmethodID stop = env->GetStaticMethodID(cls, "stop", "(Landroid/content/Context;)V");
	if (stop) {
		jobject activity = mcp_get_activity(env);
		if (activity) {
			env->CallStaticVoidMethod(cls, stop, activity);
		}
	}
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
	}
	env->DeleteLocalRef(cls);
}

#else

void mcp_android_notification_on() {}
void mcp_android_notification_off() {}

#endif // ANDROID_ENABLED