package org.opencrown.mobile;

import org.libsdl.app.SDLActivity;

// SDL's activity (zlib, from the pinned SDL 3 source) runs SDL_main from libmain.so: tools/mobile_run/main.cpp.
// LauncherActivity starts it with the command line (eboot, --app0 folders, --root) in EXTRA_ARGS.
public class OpenCrownActivity extends SDLActivity {
    static final String EXTRA_ARGS = "org.opencrown.mobile.ARGS";

    // SDL main() is running in this process: the launcher must bring this activity back, never start a second game.
    static boolean gameRunning() {
        return mActivityCreated && !mSDLMainFinished;
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
