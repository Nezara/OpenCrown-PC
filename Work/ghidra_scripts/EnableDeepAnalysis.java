// Pre-script: turn on the more aggressive analyzers for a second pass.
import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;

public class EnableDeepAnalysis extends GhidraScript {
    @Override
    public void run() throws Exception {
        Options o = currentProgram.getOptions("Analyzers");
        for (String name : o.getOptionNames()) {
            if (name.equals("Aggressive Instruction Finder") || name.equals("Decompiler Parameter ID")
                    || name.equals("Shared Return Calls") || name.equals("Non-Returning Functions - Discovered")) {
                o.setBoolean(name, true);
                println("enabled: " + name);
            }
        }
        o.setBoolean("Aggressive Instruction Finder.Create Analysis Bookmarks", false);
    }
}
