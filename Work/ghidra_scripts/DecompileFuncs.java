// Decompile functions (by name or hex address) into one C file.
// Args: <output path> <name|0xaddr> [...]
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DecompileFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                String a = args[i];
                Function f = null;
                if (a.startsWith("0x")) {
                    f = getFunctionContaining(toAddr(Long.parseLong(a.substring(2), 16)));
                } else {
                    for (Symbol s : currentProgram.getSymbolTable().getSymbols(a))
                        if (s.getSymbolType() == SymbolType.FUNCTION) { f = (Function) s.getObject(); break; }
                }
                if (f == null) { pw.println("// NOT FOUND: " + a + "\n"); continue; }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                pw.println("// " + f.getEntryPoint() + " " + f.getName(true));
                pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
        }
        di.dispose();
    }
}
