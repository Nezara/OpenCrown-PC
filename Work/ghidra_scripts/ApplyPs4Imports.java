// Label PS4 GOT import slots and rename the PLT stubs that jump through them.
// Arg 0: path to imports.tsv (hex GOT address \t name \t library \t nid)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.PointerDataType;
import java.io.*;
import java.util.*;

public class ApplyPs4Imports extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        BufferedReader br = new BufferedReader(new FileReader(args[0]));
        SymbolTable st = currentProgram.getSymbolTable();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();
        int slots = 0, stubs = 0;
        String line;
        while ((line = br.readLine()) != null) {
            String[] p = line.split("\t");
            Address got = toAddr(Long.parseLong(p[0], 16));
            String name = p[1], lib = p[2];
            Namespace ns = st.getNamespace(lib, null);
            if (ns == null) ns = st.createNameSpace(null, lib, SourceType.IMPORTED);
            // label the GOT slot
            listing.clearCodeUnits(got, got.add(7), false);
            try { listing.createData(got, PointerDataType.dataType); } catch (Exception e) {}
            st.createLabel(got, "__imp_" + name, ns, SourceType.IMPORTED);
            setEOLComment(got, lib + " NID " + p[3]);
            slots++;
            // find the PLT stub(s): instructions referencing the slot
            for (Reference ref : rm.getReferencesTo(got)) {
                Address from = ref.getFromAddress();
                Instruction ins = listing.getInstructionAt(from);
                if (ins == null || !ins.getMnemonicString().equals("JMP")) continue;
                Function f = listing.getFunctionContaining(from);
                if (f == null || !f.getEntryPoint().equals(from)) {
                    if (f != null) continue; // jmp inside a larger function, not a stub
                    f = createFunction(from, name);
                    if (f == null) continue;
                }
                f.setName(name, SourceType.IMPORTED);
                f.setParentNamespace(ns);
                stubs++;
            }
        }
        br.close();
        println("labeled " + slots + " GOT slots, renamed " + stubs + " PLT stubs");
    }
}
