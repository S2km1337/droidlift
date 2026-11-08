#ifndef STORAGE_H
#define STORAGE_H

/* The game's storage folder: <app storage>/<name>/, with a trailing slash
 * (SDL's pref path: ~/Library/Application Support/<name>/ on the Mac, the
 * app container's Application Support on iOS). Created if missing. NULL,
 * with a message, if there is none. */
const char *dl_storage_dir(const char *name);

/* Creates data_dir and, as Android does for every app, its files/ and cache/
 * folders (getFilesDir, getCacheDir). Returns 0, or -1 with a message. */
int dl_storage_prepare(const char *data_dir);

#endif
