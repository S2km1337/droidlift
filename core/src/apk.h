#ifndef APK_H
#define APK_H

/* Native libraries inside the APK, in the order Java's System.loadLibrary loads them. */
#ifndef APK_LIB_DIR
#define APK_LIB_DIR "lib/armeabi-v7a"
#endif

/* Unpacks the APK's native libraries into out_dir (a git-ignored directory)
 * with the system unzip. Dev-Mac only: the iOS build bundles pre-unpacked data. */
int apk_unpack_libs(const char *apk_path, const char *out_dir);

/* The game's data (pkcmn.pak, FMOD banks), read through AAssetManager. */
#define APK_ASSET_DIR "assets"

/* Unpacks the APK's assets/ into out_dir, keeping files already there. */
int apk_unpack_assets(const char *apk_path, const char *out_dir);

#endif
