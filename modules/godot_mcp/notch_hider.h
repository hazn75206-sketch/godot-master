#ifndef GODOT_NOTCH_HIDER_H
#define GODOT_NOTCH_HIDER_H

// Hide-notch toggle (Editor Settings: android/hide_display_cutout).
// Implemented via layoutInDisplayCutoutMode on the Android activity window.
void notch_hider_register_settings();
void notch_hider_apply();

#endif // GODOT_NOTCH_HIDER_H
