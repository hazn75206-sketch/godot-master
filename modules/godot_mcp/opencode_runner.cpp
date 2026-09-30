#include "opencode_runner.h"

#include "core/object/property_info.h"
#include "core/variant/variant.h"
#include "core/io/json.h"
#include "editor/settings/editor_settings.h"

#ifdef ANDROID_ENABLED
#include "platform/android/jni_utils.h"
#include "platform/android/java_godot_wrapper.h"
#include "platform/android/os_android.h"
#endif

#define OPENCODE_DEFAULT_MODEL "opencode/muse-spark-1.3-contributor-free"
#define OPENCODE_DEFAULT_MCP_URL "http://127.0.0.1:8766/mcp"
#define OPENCODE_RUN_TIMEOUT_SEC 180

static String opencode_get_model() {
	EditorSettings *es = EditorSettings::get_singleton();
	if (es && es->has_setting("opencode/model")) {
		return String(es->get_setting("opencode/model"));
	}
	return String(OPENCODE_DEFAULT_MODEL);
}

static String opencode_get_mcp_url() {
	EditorSettings *es = EditorSettings::get_singleton();
	if (es && es->has_setting("opencode/mcp_url")) {
		return String(es->get_setting("opencode/mcp_url"));
	}
	return String(OPENCODE_DEFAULT_MCP_URL);
}

void opencode_runner_register_settings() {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return;
	}
	if (!es->has_setting("opencode/model")) {
		es->set_setting("opencode/model", String(OPENCODE_DEFAULT_MODEL));
	}
	if (!es->has_setting("opencode/mcp_url")) {
		es->set_setting("opencode/mcp_url", String(OPENCODE_DEFAULT_MCP_URL));
	}
	es->add_property_hint(PropertyInfo(Variant::STRING, "opencode/model", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "Model gratis opencode untuk agent dalam editor (mis. opencode/muse-spark-1.3-contributor-free)."));
	es->add_property_hint(PropertyInfo(Variant::STRING, "opencode/mcp_url", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "URL MCP Godot untuk agent dalam editor (loopback)."));
}

#ifdef ANDROID_ENABLED
static String opencode_jni_string(JNIEnv *env, jstring p_s) {
	if (!p_s) {
		return String();
	}
	const char *c = env->GetStringUTFChars(p_s, nullptr);
	String r(c ? c : "");
	if (c) {
		env->ReleaseStringUTFChars(p_s, c);
	}
	env->DeleteLocalRef(p_s);
	return r;
}

static jobject opencode_get_activity(JNIEnv *env) {
	OS_Android *os = OS_Android::get_singleton();
	if (!os || !os->get_godot_java()) {
		return nullptr;
	}
	return os->get_godot_java()->get_activity();
}
#endif // ANDROID_ENABLED

Dictionary opencode_runner_status() {
	Dictionary st;
	st["model"] = opencode_get_model();
	st["mcp_url"] = opencode_get_mcp_url();
#ifdef ANDROID_ENABLED
	JNIEnv *env = get_jni_env();
	bool installed = false;
	String version;
	if (env) {
		jclass cls = jni_find_class(env, "org/godotengine/godot/opencode/OpencodeRunner");
		if (cls) {
			OS_Android *os = OS_Android::get_singleton();
			jobject activity = (os && os->get_godot_java()) ? os->get_godot_java()->get_activity() : nullptr;
			if (activity) {
				jmethodID m_inst = env->GetStaticMethodID(cls, "isInstalled", "(Landroid/content/Context;)Z");
				if (m_inst) {
					installed = env->CallStaticBooleanMethod(cls, m_inst, activity);
				}
				jmethodID m_ver = env->GetStaticMethodID(cls, "getVersion", "(Landroid/content/Context;)Ljava/lang/String;");
				if (m_ver) {
					version = opencode_jni_string(env, (jstring)env->CallStaticObjectMethod(cls, m_ver, activity));
				}
			}
			if (env->ExceptionCheck()) {
				env->ExceptionClear();
			}
			env->DeleteLocalRef(cls);
		}
	}
	st["installed"] = installed;
	st["version"] = version;
#else
	st["installed"] = false;
	st["version"] = String();
#endif // ANDROID_ENABLED
	return st;
}

Dictionary opencode_runner_run(const String &p_prompt) {
	Dictionary res;
	res["output"] = String();
	res["error"] = String("opencode hanya tersedia di build Android.");
#ifdef ANDROID_ENABLED
	JNIEnv *env = get_jni_env();
	if (!env) {
		res["error"] = String("JNI tidak tersedia.");
		return res;
	}
	jclass cls = jni_find_class(env, "org/godotengine/godot/opencode/OpencodeRunner");
	if (!cls) {
		res["error"] = String("Kelas OpencodeRunner tidak ditemukan.");
		return res;
	}
	jmethodID m_run = env->GetStaticMethodID(cls, "runPrompt", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
	if (!m_run) {
		env->DeleteLocalRef(cls);
		res["error"] = String("Metode runPrompt tidak ditemukan.");
		return res;
	}
	jobject activity = opencode_get_activity(env);
	if (!activity) {
		env->DeleteLocalRef(cls);
		res["error"] = String("Activity tidak tersedia.");
		return res;
	}
	String model = opencode_get_model();
	String mcp_url = opencode_get_mcp_url();
	jstring j_prompt = env->NewStringUTF(p_prompt.utf8().get_data());
	jstring j_model = env->NewStringUTF(model.utf8().get_data());
	jstring j_url = env->NewStringUTF(mcp_url.utf8().get_data());
	jstring j_res = (jstring)env->CallStaticObjectMethod(cls, m_run, activity, j_prompt, j_model, j_url, (jint)OPENCODE_RUN_TIMEOUT_SEC);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		res["error"] = String("Pemanggilan Java gagal.");
	} else if (j_res) {
		String raw = opencode_jni_string(env, j_res);
		// Java returns {"output": ..., "error": ...}; parse the output field.
		Variant parsed = JSON::parse_string(raw);
		if (parsed.get_type() == Variant::DICTIONARY) {
			Dictionary d = parsed;
			res["output"] = d.get("output", String());
			res["error"] = d.get("error", String());
		} else {
			res["output"] = raw;
		}
	}
	env->DeleteLocalRef(j_prompt);
	env->DeleteLocalRef(j_model);
	env->DeleteLocalRef(j_url);
	env->DeleteLocalRef(cls);
#endif // ANDROID_ENABLED
	return res;
}
