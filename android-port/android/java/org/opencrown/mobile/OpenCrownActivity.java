package org.opencrown.mobile;

import android.os.Build;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
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

    // SDLActivity.onCreate shows the bars (setWindowStyle(false)) and the native SDL_WINDOW_FULLSCREEN request only
    // goes through the deprecated setSystemUiVisibility, once, which modern Android (targetSdk 35) does not keep
    // after a focus change. Hide them again ourselves with the same insets API as the launcher, every time we get
    // focus back (start, Home and back, a dialog or the shade closing).
    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) {
            hideSystemBars();
            getWindow().getDecorView().post(new Runnable() { public void run() { hideSystemBars(); } });
        }
    }

    void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }
}
