package org.opencrown.mobile;

import java.util.ArrayList;
import java.util.List;

// The launcher's online calls into libmain.so (src/platform/android_launcher.cpp): Test connection, the friends
// session and Report a Bug, as on the PC launcher. They block for up to a few seconds: call them off the UI thread.
final class Net {
    static {
        System.loadLibrary("SDL3");  // libmain.so links it
        System.loadLibrary("main");
    }

    static final int NONE = 0, REQUEST = 1, ACCEPT = 2, DECLINE = 3, REMOVE = 4;

    private static native String onlineCheck(String lobby, String name, boolean upnp);
    private static native String friends(String root, String lobby, String name, int action, String arg);
    static native String bugLog(String root, String logPath);
    private static native String bugReport(String root, String lobby, String name, String note, String logPath,
                                           String version);

    static List<String[]> lines(String text) {
        List<String[]> out = new ArrayList<>();
        for (String l : text.split("\n")) if (!l.isEmpty()) out.add(l.split("\t", -1));
        return out;
    }

    // Test connection, worded as the PC launcher's Online page.
    static final class Check {
        String verdict = "", lobby = "", endpoint = "", upnp = "";
        boolean lobbyOk, stunOk, upnpTried, upnpOk;
        int natSame = -1;
    }

    static Check check(String lobby, String name, boolean upnp) {
        Check c = new Check();
        for (String[] l : lines(onlineCheck(lobby, name, upnp))) {
            if (l[0].equals("verdict")) c.verdict = l[1];
            else if (l[0].equals("lobby")) {
                c.lobbyOk = l[1].equals("1");
                c.lobby = l[2] + (l[3].isEmpty() ? "" : ", sees you as " + l[3]) + (c.lobbyOk ? " (" + l[4] + " ms)" : "");
            } else if (l[0].equals("stun")) {
                c.stunOk = l[1].equals("1");
                c.natSame = Integer.parseInt(l[5]);
                String nat = c.natSame == 1 ? "endpoint-independent (good)"
                        : c.natSame == 0 ? "symmetric (direct connections likely fail)" : "not checked";
                c.endpoint = l[3].isEmpty() ? l[2] : l[3] + ":" + l[4] + " " + l[2] + ", NAT: " + nat;
            } else if (l[0].equals("upnp")) {
                c.upnpTried = l[1].equals("1");
                c.upnpOk = l[2].equals("1");
                c.upnp = l[3];
            }
        }
        return c;
    }

    static final class Person {
        String id, name, joinCode = "";
        boolean online;
    }

    static final class Friends {
        boolean identityOk = true, ok, actionOk, acted;
        String identityError = "", error = "", code = "", actionMsg = "";
        final List<Person> friends = new ArrayList<>(), incoming = new ArrayList<>(), outgoing = new ArrayList<>();
        int online() {
            int n = 0;
            for (Person p : friends) if (p.online) n++;
            return n;
        }
    }

    static Friends session(String root, String lobby, String name, int action, String arg) {
        Friends f = new Friends();
        for (String[] l : lines(friends(root, lobby, name, action, arg == null ? "" : arg))) {
            switch (l[0]) {
                case "identity": f.identityOk = l[1].equals("1"); f.identityError = l[2]; break;
                case "state": f.ok = l[1].equals("1"); f.error = l[2]; break;
                case "code": f.code = l[1]; break;
                case "action": f.acted = true; f.actionOk = l[1].equals("1"); f.actionMsg = l[2]; break;
                case "friend": {
                    Person p = new Person();
                    p.id = l[1]; p.name = l[2]; p.online = l[3].equals("1"); p.joinCode = l[4];
                    f.friends.add(p);
                    break;
                }
                case "in": case "out": {
                    Person p = new Person();
                    p.id = l[1]; p.name = l[2];
                    (l[0].equals("in") ? f.incoming : f.outgoing).add(p);
                    break;
                }
            }
        }
        // Online friends first, then by name, as on the PC launcher.
        java.util.Collections.sort(f.friends, new java.util.Comparator<Person>() {
            public int compare(Person a, Person b) {
                return a.online == b.online ? a.name.compareToIgnoreCase(b.name) : a.online ? -1 : 1;
            }
        });
        return f;
    }

    static final class Report {
        boolean ok;
        String msg = "";
        int left = -1;
    }

    static Report report(String root, String lobby, String name, String note, String logPath, String version) {
        Report r = new Report();
        for (String[] l : lines(bugReport(root, lobby, name, note, logPath, version))) {
            if (!l[0].equals("result")) continue;
            r.ok = l[1].equals("1");
            r.msg = l[2];
            r.left = Integer.parseInt(l[3]);
        }
        return r;
    }
}
