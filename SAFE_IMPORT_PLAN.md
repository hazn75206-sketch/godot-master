# Rencana: Safe Import Mode (Godot Android Editor)

Status: **diimplementasikan + jalur headless Android ditambal, menunggu build APK + tes di HP**
Repo: `/storage/emulated/0/godot-master`
Dibuat: 2026-11-13

## Tujuan

Menambahkan mode **Import Only (Safe Mode)** untuk project Godot Android yang besar/kompleks,
supaya proses import tidak memaksa editor ikut force close di HP RAM kecil.

Prinsip:
- Import dijalankan di **proses terpisah** `:GodotImport` dan **headless**.
- Layar hanya menampilkan teks log ala terminal + spinner + progress + tombol.
- Kalau proses import mati (OOM), **Project Manager tetap hidup**.
- Tidak ada perubahan di luar fitur ini.

## Keputusan final dari user

1. Dialog recovery punya **3 metode**:
   - Edit normally
   - Edit in Recovery Mode
   - **Import Only (Safe Mode)**
2. Setelah import sukses: **otomatis kembali ke Project Manager**.
3. **Tidak** menambahkan tombol "Hapus .godot lalu coba lagi".
4. **Tidak** ada batas waktu / deteksi "macet". Sebagai gantinya ada tombol **Keluar** dan **Reload**.
5. Tombol **Keluar** saat import masih berjalan:
   - Muncul dialog peringatan: **"Yakin ingin keluar?"**
   - Hanya 2 pilihan: **Ya** dan **Tidak**.
   - Tidak ada opsi "lanjut di background".
   - Ya = hentikan import lalu kembali ke Project Manager.
   - Tidak = dialog ditutup, import lanjut.
6. Desain `.tscn` hanya **referensi visual**. UI Safe Import adalah **Android native**
   (Kotlin + XML), bukan scene Godot, karena proses engine headless.
7. Spinner dan progress terserah implementasi; tombol tetap di bawah.

## Referensi desain

- Scene contoh: `/storage/emulated/0/Documents/detail-desain/control.tscn`
  - Root `Control` full screen.
  - `Label` log memenuhi layar sampai ~77 px dari bawah.
  - `Button` "Keluar" di kiri bawah.
- Screenshot: `/storage/emulated/0/DCIM/Screenshots/Screenshot_2026-11-13-18-03-41-42_b5233b373800a64b069f8964d999de0a.jpg`

Desain itu diterjemahkan ke layout Android:

```
+------------------------------------------------------+
|  SAFE IMPORT - <nama project>                        |
|  log terminal (monospace, auto-scroll, bisa discroll)|
|  ...                                                 |
+------------------------------------------------------+
| [Keluar]   (spinner)  [====progress====] 12/120      |
|                       Mengimpor: file.tscn   [Reload]|
+------------------------------------------------------+
```

- Background gelap (`#1e1e1e`), teks `#dcdcdc`.
- Log: `ScrollView` + `TextView` monospace, padding 12 dp, auto-scroll, maksimal ~200 KB teks.
- Bar bawah tinggi ~64–72 dp:
  - Keluar (kiri)
  - Spinner (tengah-kiri)
  - Progress bar + `12/120` + nama file (tengah)
  - Reload (kanan)
- Warna status: biru = jalan, hijau = selesai, merah = gagal.

## Arsitektur

### Kenapa UI Android native, bukan UI Godot/C++

Proses import berjalan `--headless` (tanpa renderer). Kalau layar memakai UI Godot,
engine harus menyalakan renderer GL/Vulkan lagi, sehingga memori naik dan tujuan Safe Import hilang.
Karena itu:
- **Logika import + progress = C++ di engine.**
- **Layar terminal = Android native (Kotlin + XML).**

### Alur

1. User pilih project di Project Manager.
2. Kalau muncul dialog recovery, pilih **Import Only (Safe Mode)**.
3. Project Manager memanggil Java untuk start `GodotImportActivity` di proses `:GodotImport`.
4. Activity menjalankan engine dengan argumen:
   `--headless --import --path <project> --log-file <path> --safe-import`
5. Engine import memakai semua core (tanpa cap thread).
6. Activity membaca log file, menampilkan log, progress, dan memutar spinner.
7. Sukses: tampil "Import selesai" ±1 detik, lalu Activity ditutup dan kembali ke Project Manager.
8. Gagal: tetap di layar; tombol Reload / Keluar aktif.
9. Keluar saat berjalan: dialog "Yakin ingin keluar?" (Ya/Tidak).
10. Reload: restart proses import dari awal.

## Implementasi

### A. Android UI + proses import

File baru:
- `platform/android/java/editor/src/main/java/org/godotengine/editor/GodotImportActivity.kt`
- `platform/android/java/editor/src/main/res/layout/godot_import_layout.xml`
- `platform/android/java/editor/src/main/res/values/strings.xml` (tambahan string)

`GodotImportActivity`:
- Extends `Activity` + implement `GodotHost`.
- **Jangan** pakai `GodotActivity` / `GodotFragment`, karena keduanya memanggil
  `Godot.onInitRenderView()` dan mensyaratkan renderer (`vulkan`/`opengl3`).
- Alur:
  1. Ambil extra: project path, log path.
  2. Set content view terminal.
  3. `Godot.getInstance(applicationContext).initEngine(this, args)`.
  4. Jalankan thread khusus:
     - loop `GodotLib.step()` sampai `Godot.RunStatus.TERMINATING` (engine selesai/quit).
     - beri jeda 1 ms saat frame tidak swap agar tidak spin penuh.
     - setelah selesai: `GodotLib.ondestroy()`.
  5. Update UI lewat `runOnUiThread`.
- `getCommandLine()` mengembalikan:
  `--headless`, `--import`, `--path`, `<project>`, `--log-file`, `<log path>`, `--safe-import`
- `getGodot()` / `getActivity()` wajib diimplementasikan.
- `onGodotSetupCompleted()` / `onGodotMainLoopStarted()`: update status "Mengimpor...".
- Keep screen on selama import.
- Back button = sama dengan Keluar (dengan dialog konfirmasi).

Manifest:
- Tambah activity `GodotImportActivity`:
  - `android:process=":GodotImport"`
  - `android:exported="false"`
  - theme gelap
  - `android:configChanges="layoutDirection|locale|orientation|keyboardHidden|screenSize|smallestScreenSize|density|keyboard|navigation|screenLayout|uiMode|fontScale|fontWeightAdjustment"`
  - `android:screenOrientation="userLandscape"`
  - `android:excludeFromRecents="true"` (opsional)

### B. Spinner, progress, log

- `LogTailer` (Kotlin):
  - Baca `--log-file` setiap 200 ms dari offset terakhir.
  - Kirim baris baru ke UI.
  - Parse baris:
    - `SAFE_IMPORT_BEGIN <total>`
    - `SAFE_IMPORT_PROGRESS <index>/<total> <file>`
    - `SAFE_IMPORT_FILE_FAILED <file>`
    - `SAFE_IMPORT_DONE`
- `HeartbeatSpinner` (custom View):
  - Rotasi mengikuti progres (setiap `SAFE_IMPORT_PROGRESS`).
  - **Tanpa timer** dan **tanpa pesan macet**.
  - Kalau tidak ada progres, spinner berhenti; user bisa Reload/Keluar.
- Progress bar determinate: `max = total`, `progress = index`.
- Teks status: `Mengimpor <index>/<total> - <nama file>`.
- Log `TextView`:
  - Monospace, auto-scroll ke bawah.
  - Maksimal 2000 baris; buang baris paling lama.

### C. Peluncuran dari Project Manager

- `GodotJavaWrapper` + `platform/android/java/lib/src/main/java/org/godotengine/godot/Godot.kt`:
  - Tambah method `startSafeImport(projectPath: String)`; activity menentukan path log sendiri.
  - Start `GodotImportActivity` lewat `Intent().setClassName(...)` agar modul `lib`
    tidak perlu import langsung kelas editor.
- `editor/project_manager/project_manager.cpp`:
  - Tambah handler tombol ketiga di dialog recovery.
  - `_on_recovery_mode_popup_open_safe_import()`:
    - ambil project path terpilih
    - panggil `OS_Android` / `GodotJavaWrapper` start safe import
    - tutup dialog
- `editor/project_manager/project_list.cpp` (opsional):
  - Menu context `MENU_SAFE_IMPORT` = "Import Only (Safe Mode)".
- Dialog recovery saat ini:
  - Tombol "Edit normally"
  - OK = "Edit in Recovery Mode"
  - Tambah tombol ketiga "Import Only (Safe Mode)"

### D. Perubahan native/C++

- `main/main.cpp`:
  - Parse flag `--safe-import` (dipakai bersama `--import`).
  - Simpan status agar bisa dibaca `EditorFileSystem`.
- `editor/file_system/editor_file_system.cpp` + `editor/editor_node.cpp`:
  - Saat safe import, print progress ke stdout/log:
    - `SAFE_IMPORT_BEGIN <total>`
    - `SAFE_IMPORT_PROGRESS <index>/<total> <file>`
    - `SAFE_IMPORT_FILE_FAILED <file>` (kalau import file gagal)
    - `SAFE_IMPORT_DONE`
  - Tidak mengubah cara import, tidak mengubah threading.
  - `EditorProgress::step()` memancarkan `SAFE_IMPORT_PROGRESS` untuk task `reimport`.
  - Tetap pakai semua core:
    - `WorkerThreadPool` editor saat ini `init(-1, 0.75)`.
    - `editor/import/use_multiple_threads` default `true`.

### E. Perilaku tombol

- **Keluar**:
  - Jika import selesai/gagal: langsung kembali ke Project Manager.
  - Jika import masih berjalan:
    - Tampilkan dialog: **"Yakin ingin keluar?"**
    - Tombol: **Ya** / **Tidak** (hanya 2).
    - Ya: hentikan proses import, kembali ke Project Manager.
    - Tidak: tutup dialog, lanjut import.
- **Reload**:
  - Konfirmasi (opsional), lalu restart proses import dari awal.
  - Gunakan `ProcessPhoenix.triggerRebirth(...)` agar proses `:GodotImport` benar-benar baru
    (menghindari reuse state native Godot).
- **Sukses**:
  - Tampilkan status "Import selesai".
  - Delay ±1 detik, lalu `finishAndRemoveTask()`.
  - Project Manager (proses lain) tetap hidup dan kembali tampil.
- **Gagal**:
  - Tetap di layar.
  - Tampilkan error terakhir.
  - Reload / Keluar aktif.

## Di luar scope (jangan dikerjakan)

- Crash logging / toggle log.
- Low-memory mode otomatis.
- JNI batching `DirAccess` / `FileAccess`.
- Perubahan texture import.
- Penghapusan `.godot` / tombol repair cache.
- Timeout "macet" dan pesan stuck.
- Perubahan worker thread / thread cap.
- Perubahan pada project game.

## Risiko utama

1. **`Godot.initEngine` + `GodotLib.step()` tanpa render view** harus diuji di HP.
   Ini titik paling riskan. Sudah ditambal di `OS_Android::main_loop_iterate()`:
   kalau `DisplayServerAndroid` null (mode `--headless`), langsung panggil `Main::iteration()`
   tanpa surface. Tetap perlu tes perangkat.
2. **Max speed = peak RAM tinggi.** Kalau proses import OOM, proses import mati,
   tapi Project Manager tetap hidup.
3. **Keluar di tengah import** bisa meninggalkan `.godot` setengah jadi.
   Karena user menolak fitur hapus cache, dialog "Yakin ingin keluar?" adalah peringatannya.
4. **Auto-return ke Project Manager** bergantung pada proses Project Manager masih hidup.
   Biasanya tetap hidup karena beda proses dan sedang background.

## Verifikasi

1. Build APK lewat GitHub Actions / lokal.
2. Tes project kecil (Blood Fire) dulu:
   - Safe Import dari dialog recovery.
   - Log tampil, spinner/progress jalan.
   - Selesai → otomatis kembali ke Project Manager.
   - Buka project normal, tidak force close.
3. Tes project besar di HP 3 GB:
   - Pastikan Project Manager selamat kalau proses import dibunuh OOM.
   - Reload mengulang import dari awal.
   - Keluar saat berjalan memunculkan dialog Ya/Tidak.
4. Cek `.godot/imported` terupdate dan `--import` keluar normal.
5. Pastikan alur editor normal (non-Safe Import) tidak berubah.

## Checklist implementasi

- [x] `GodotImportActivity.kt` + loop `GodotLib.step()`
- [x] Jalur headless `OS_Android::main_loop_iterate()` tanpa `DisplayServerAndroid`
- [x] Layout `godot_import_layout.xml` (log + spinner + progress + tombol)
- [x] `LogTailer` + parser `SAFE_IMPORT_*`
- [x] `SafeImportSpinner` (spinner digerakkan progres)
- [x] Manifest activity `:GodotImport`
- [x] `GodotJavaWrapper.startSafeImport` + `Godot.kt`
- [x] Tombol ketiga di dialog recovery Project Manager
- [ ] (Opsional) menu context ProjectList (tidak dipakai; alur lewat dialog recovery)
- [x] `--safe-import` di `main.cpp`
- [x] Progress lines di `editor_file_system.cpp` + `EditorProgress::step()`
- [x] Dialog "Yakin ingin keluar?" (Ya/Tidak)
- [x] Reload via `ProcessPhoenix`
- [x] Auto-return setelah sukses
- [ ] Build APK (butuh SDK/Gradle/SCons; belum tersedia di environment ini)
- [ ] Tes di HP 3 GB + project besar
