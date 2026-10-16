// Export a queryable index of the program so agents never need Ghidra open for lookups.
// Arg 0: output directory. Writes functions.tsv, calls.tsv, strings.tsv there.
//   functions.tsv: entry  size  name  ncallers  ncallees
//   calls.tsv:     caller_entry  callee_entry   (one line per distinct edge)
//   strings.tsv:   string_addr  ref_function_entries(comma)  text   (only strings referenced from code)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportIndex extends GhidraScript {
    @Override
    public void run() throws Exception {
        File dir = new File(getScriptArgs()[0]);
        dir.mkdirs();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        try (PrintWriter fw = new PrintWriter(new FileWriter(new File(dir, "functions.tsv")));
             PrintWriter cw = new PrintWriter(new FileWriter(new File(dir, "calls.tsv")))) {
            for (Function f : fm.getFunctions(true)) {
                if (monitor.isCancelled()) break;
                Set<Function> callees = f.getCalledFunctions(monitor);
                Set<Function> callers = f.getCallingFunctions(monitor);
                fw.printf("%x\t%d\t%s\t%d\t%d%n", f.getEntryPoint().getOffset(), f.getBody().getNumAddresses(),
                          f.getName(true), callers.size(), callees.size());
                for (Function c : callees)
                    cw.printf("%x\t%x%n", f.getEntryPoint().getOffset(), c.getEntryPoint().getOffset());
            }
        }

        int n = 0;
        try (PrintWriter sw = new PrintWriter(new FileWriter(new File(dir, "strings.tsv")))) {
            for (Data d : currentProgram.getListing().getDefinedData(true)) {
                if (!d.hasStringValue()) continue;
                TreeSet<Long> fns = new TreeSet<>();
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    if (f != null) fns.add(f.getEntryPoint().getOffset());
                }
                if (fns.isEmpty()) continue;
                String s = StringDataInstance.getStringDataInstance(d).getStringValue();
                if (s == null) continue;
                StringBuilder sb = new StringBuilder();
                for (long a : fns) sb.append(sb.length() == 0 ? "" : ",").append(Long.toHexString(a));
                sw.printf("%x\t%s\t%s%n", d.getAddress().getOffset(), sb,
                          s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r"));
                n++;
            }
        }
        println("exported " + fm.getFunctionCount() + " functions, " + n + " referenced strings -> " + dir);
    }
}
