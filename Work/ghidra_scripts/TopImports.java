// List the most-referenced import stubs and the callers of a few anchor strings.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.Address;
import java.util.*;

public class TopImports extends GhidraScript {
    @Override
    public void run() throws Exception {
        ReferenceManager rm = currentProgram.getReferenceManager();
        List<String> rows = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getSymbol().getSource() != SourceType.IMPORTED) continue;
            int n = rm.getReferenceCountTo(f.getEntryPoint());
            rows.add(String.format("%5d %s::%s @%s", n, f.getParentNamespace().getName(), f.getName(), f.getEntryPoint()));
        }
        rows.sort((a, b) -> Integer.parseInt(b.substring(0,5).trim()) - Integer.parseInt(a.substring(0,5).trim()));
        for (int i = 0; i < 25 && i < rows.size(); i++) println(rows.get(i));
        for (String s : new String[]{"Start Act_SetupScene", "/app0/DRACRO_US.cpk", "VANILLAWARE_LOGO_DISP"}) {
            Address a = findBytes(null, s);
            if (a == null) { println("not found: " + s); continue; }
            for (Reference r : rm.getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                println("'" + s + "' used by " + (f == null ? "?" : f.getName()) + " at " + r.getFromAddress());
            }
        }
    }
}
