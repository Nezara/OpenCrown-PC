package org.opencrown.mobile;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

// SDL runs main() once per process (SDLActivity.onCreate exits the process when it is re-created after main() has
// returned), so Play after a failed start needs a fresh process. This activity lives in its own process (":restart"):
// the launcher starts it and exits, it stops the old process, starts the game (in a new main process) and exits too.
public class RestartActivity extends Activity {
    static final String EXTRA_NEXT = "org.opencrown.mobile.NEXT", EXTRA_PID = "org.opencrown.mobile.PID";

    static void restartInto(Activity from, Intent next) {
        Intent i = new Intent(from, RestartActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        i.putExtra(EXTRA_NEXT, next);
        i.putExtra(EXTRA_PID, Process.myPid());
        from.startActivity(i);
        from.finish();
        Runtime.getRuntime().exit(0);
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Process.killProcess(getIntent().getIntExtra(EXTRA_PID, -1));  // usually gone already
        Intent next = getIntent().getParcelableExtra(EXTRA_NEXT);
        if (next != null) {
            next.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(next);
        }
        finish();
        Runtime.getRuntime().exit(0);
    }
}
