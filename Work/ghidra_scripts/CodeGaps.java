// Report the largest undefined (no instruction/data) ranges in the executable block.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import java.util.*;

public class CodeGaps extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock b = currentProgram.getMemory().getBlock("segment_0");
        Listing l = currentProgram.getListing();
        AddressSet defined = new AddressSet();
        for (Instruction i : l.getInstructions(new AddressSet(b.getStart(), b.getEnd()), true)) defined.add(i.getMinAddress(), i.getMaxAddress());
        for (Data d : l.getDefinedData(new AddressSet(b.getStart(), b.getEnd()), true)) defined.add(d.getMinAddress(), d.getMaxAddress());
        AddressSet gaps = new AddressSet(b.getStart(), b.getEnd()).subtract(defined);
        println("undefined bytes in segment_0: 0x" + Long.toHexString(gaps.getNumAddresses()) + " in " + gaps.getNumAddressRanges() + " ranges");
        List<AddressRange> rs = new ArrayList<>(); for (AddressRange r : gaps) rs.add(r);
        rs.sort((x, y) -> Long.compare(y.getLength(), x.getLength()));
        for (int i = 0; i < 12 && i < rs.size(); i++) {
            AddressRange r = rs.get(i);
            byte[] s = new byte[16]; currentProgram.getMemory().getBytes(r.getMinAddress(), s);
            StringBuilder h = new StringBuilder(); for (byte x : s) h.append(String.format("%02x ", x));
            println(String.format("%s-%s len=0x%x  %s", r.getMinAddress(), r.getMaxAddress(), r.getLength(), h));
        }
        long small = 0; for (AddressRange r : rs) if (r.getLength() < 64) small += r.getLength();
        println("bytes in ranges <64 bytes (alignment padding): 0x" + Long.toHexString(small));
    }
}
