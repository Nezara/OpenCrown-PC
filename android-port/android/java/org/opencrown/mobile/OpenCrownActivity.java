package org.opencrown.mobile;

import android.content.Intent;

import org.libsdl.app.SDLActivity;

// SDL's activity (zlib, from the pinned SDL 3 source) runs SDL_main from libmain.so: tools/mobile_run/main.cpp.
// LauncherActivity starts it with the command line (eboot, --app0 folders, --root) in EXTRA_ARGS.
public class OpenCrownActivity extends SDLActivity {
    static final String EXTRA_ARGS = "org.opencrown.mobile.ARGS";

    // SDL main() is running in this process: the launcher must bring this activity back, never start a second game.
    static boolean gameRunning() {
        return mActivityCreated && !mSDLMainFinished;
    }

    // main() returned: the game stopped before it ran (a normal quit ends the process in main.cpp with _Exit).
    // Starting it again needs a new process (RestartActivity).
    static boolean mainFinished() {
        return mSDLMainFinished;
    }

    // SDLMain calls finish() when main() returns, i.e. the game could not start (bad dump, load error). Back to the
    // launcher, which shows why (the last "mobile_run:" line of log.txt), instead of dropping to the home screen.
    @Override
    public void finish() {
        if (mSDLMainFinished && getArguments().length > 0) {
            Intent i = new Intent(this, LauncherActivity.class);
            i.putExtra(LauncherActivity.EXTRA_GAME_FAILED, true);
            startActivity(i);
        }
        super.finish();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected String[] getArguments() {
        String[] a = getIntent() != null ? getIntent().getStringArrayExtra(EXTRA_ARGS) : null;
        return a != null ? a : new String[0];
    }
}
