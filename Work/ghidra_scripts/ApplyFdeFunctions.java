// Create functions at every .eh_frame FDE start address that Ghidra has not already got one at.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.*;

public class ApplyFdeFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        BufferedReader br = new BufferedReader(new FileReader(getScriptArgs()[0]));
        Listing l = currentProgram.getListing();
        int created = 0, existing = 0, split = 0, failed = 0; String line;
        while ((line = br.readLine()) != null) {
            Address a = toAddr(Long.parseLong(line.trim(), 16));
            if (l.getFunctionAt(a) != null) { existing++; continue; }
            Function container = l.getFunctionContaining(a);
            if (l.getInstructionAt(a) == null && l.getDefinedDataAt(a) == null) disassemble(a);
            if (createFunction(a, null) != null) { created++; if (container != null) split++; } else failed++;
        }
        br.close();
        println("existing=" + existing + " created=" + created + " (of which split from a containing function=" + split + ") failed=" + failed);
        println("functions now=" + currentProgram.getFunctionManager().getFunctionCount());
    }
}
