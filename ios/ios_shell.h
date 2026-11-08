/*
 * The iOS app shell: what the loader's command line says on the
 * Mac, the iOS app says here.
 */
#ifndef IOS_SHELL_H
#define IOS_SHELL_H

/* Sets the SDL hints for iOS (landscape only, touch, the audio session) and
 * replaces argv with the app's own arguments: the libraries and assets
 * unpacked into the app bundle at build time (--unpacked --work
 * <bundle>/apk), ANGLE from the bundle's Frameworks folder, and the game in
 * a full-screen window (--init-test --window). Arguments given at launch
 * (xcrun simctl launch DEVICE BUNDLE_ID --frames 900 ...) follow them, so
 * they can add to or override them; Apple's own "-Key value" launch
 * arguments are dropped. */
void ios_shell_args(int *argc, char ***argv);

/* Before main returns or _exits: the log's last lines reach its file and
 * os_log (ios_log.h). */
void ios_shell_exit(void);

#endif
