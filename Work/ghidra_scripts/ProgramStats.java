// Print a quick summary of the analyzed program.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class ProgramStats extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks())
            println(String.format("block %-10s %s-%s size=0x%x %s%s%s", b.getName(), b.getStart(), b.getEnd(), b.getSize(),
                b.isRead()?"r":"-", b.isWrite()?"w":"-", b.isExecute()?"x":"-"));
        FunctionManager fm = currentProgram.getFunctionManager();
        int n = 0, thunks = 0, named = 0; long bytes = 0;
        for (Function f : fm.getFunctions(true)) { n++; if (f.isThunk()) thunks++; if (f.getSymbol().getSource() != SourceType.DEFAULT) named++; bytes += f.getBody().getNumAddresses(); }
        println("functions=" + n + " thunks=" + thunks + " named=" + named + " codeBytes=0x" + Long.toHexString(bytes));
        println("instructions=" + currentProgram.getListing().getNumInstructions());
        println("definedData=" + currentProgram.getListing().getNumDefinedData());
        int strings = 0; for (Data d : currentProgram.getListing().getDefinedData(true)) if (d.hasStringValue()) strings++;
        println("strings=" + strings);
        println("entry=" + currentProgram.getSymbolTable().getSymbols("entry").hasNext());
    }
}
