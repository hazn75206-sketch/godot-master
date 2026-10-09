#include "notch_hider.h"

#include "core/error/error_macros.h"
#include "core/object/property_info.h"
#include "core/string/ustring.h"
#include "editor/settings/editor_settings.h"

#ifdef ANDROID_ENABLED
#include "platform/android/jni_utils.h"
#include "platform/android/java_godot_wrapper.h"
#include "platform/android/os_android.h"
#endif

// Peringatan kegagalan JNI: sekali per sebab sampai sukses lagi,
// agar tidak spam tiap poll tapi tetap terlihat di logcat.
static String notch_last_warn;

static void notch_warn_once(const String &p_why) {
	if (notch_last_warn == p_why) {
		return;
	}
	notch_last_warn = p_why;
	WARN_PRINT(vformat("NotchHider: %s (cutout tidak diterapkan)", p_why));
}

static void notch_warn_reset() {
	notch_last_warn = String();
}

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
	// Tanpa cache sekali-pakai: window attrs bisa di-reset Godot/activity
	// kapan saja (resume, rotasi, edge-to-edge), jadi terapkan tiap poll.
	EditorSettings *es = EditorSettings::get_singleton();
	bool hidden = true;
	if (es && es->has_setting("android/hide_display_cutout")) {
		hidden = bool(es->get_setting("android/hide_display_cutout"));
	}
	JNIEnv *env = get_jni_env();
	if (!env) {
		notch_warn_once("get_jni_env null");
		return;
	}
	jclass cls = jni_find_class(env, "org/godotengine/godot/notch/NotchHider");
	if (!cls) {
		notch_warn_once("kelas NotchHider tidak ketemu");
		return;
	}
	jmethodID set_hidden = env->GetStaticMethodID(cls, "setHidden", "(Landroid/content/Context;Z)V");
	if (!set_hidden) {
		notch_warn_once("method setHidden tidak ketemu");
		env->DeleteLocalRef(cls);
		return;
	}
	OS_Android *os = OS_Android::get_singleton();
	jobject activity = (os && os->get_godot_java()) ? os->get_godot_java()->get_activity() : nullptr;
	if (!activity) {
		notch_warn_once("activity null");
		env->DeleteLocalRef(cls);
		return;
	}
	env->CallStaticVoidMethod(cls, set_hidden, activity, (jboolean)hidden);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		notch_warn_once("exception saat setHidden");
	} else {
		notch_warn_reset();
	}
	env->DeleteLocalRef(cls);
#else
	(void)0;
#endif // ANDROID_ENABLED
}
