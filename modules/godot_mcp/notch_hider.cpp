#include "notch_hider.h"

#include "core/object/property_info.h"
#include "editor/settings/editor_settings.h"

#ifdef ANDROID_ENABLED
#include "platform/android/jni_utils.h"
#include "platform/android/java_godot_wrapper.h"
#include "platform/android/os_android.h"
#endif

// Cache last applied value so the 2-second poll only touches JNI on change.
static bool notch_last_applied = false;
static bool notch_has_applied = false;

void notch_hider_register_settings() {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return;
	}
	if (!es->has_setting("android/hide_display_cutout")) {
		es->set_setting("android/hide_display_cutout", true);
	}
	es->add_property_hint(PropertyInfo(Variant::BOOL, "android/hide_display_cutout", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "Layar penuh: pakai seluruh layar termasuk area poni (notch), tanpa bar hitam letterbox. Bisa dimatikan."));
}

void notch_hider_apply() {
#ifdef ANDROID_ENABLED
	EditorSettings *es = EditorSettings::get_singleton();
	bool hidden = true;
	if (es && es->has_setting("android/hide_display_cutout")) {
		hidden = bool(es->get_setting("android/hide_display_cutout"));
	}
	if (notch_has_applied && hidden == notch_last_applied) {
		return;
	}
	JNIEnv *env = get_jni_env();
	if (!env) {
		return;
	}
	jclass cls = jni_find_class(env, "org/godotengine/godot/notch/NotchHider");
	if (!cls) {
		return;
	}
	jmethodID set_hidden = env->GetStaticMethodID(cls, "setHidden", "(Landroid/content/Context;Z)V");
	if (set_hidden) {
		OS_Android *os = OS_Android::get_singleton();
		jobject activity = (os && os->get_godot_java()) ? os->get_godot_java()->get_activity() : nullptr;
		if (activity) {
			env->CallStaticVoidMethod(cls, set_hidden, activity, (jboolean)hidden);
			notch_last_applied = hidden;
			notch_has_applied = true;
		}
	}
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
	}
	env->DeleteLocalRef(cls);
#else
	(void)notch_last_applied;
	(void)notch_has_applied;
#endif // ANDROID_ENABLED
}
