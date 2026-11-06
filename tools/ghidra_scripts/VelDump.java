// Ghidra post-script: locate velext's memory-write path and dump a call map.
// Output goes to the headless log.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class VelDump extends GhidraScript {

    private void dumpBefore(Instruction ins, int n) {
        Instruction p = ins.getPrevious();
        List<String> rows = new ArrayList<>();
        for (int i = 0; i < n && p != null; i++) {
            rows.add(0, String.format("        %s  %s", p.getAddress(), p.toString()));
            p = p.getPrevious();
        }
        for (String r : rows) println(r);
    }

    private long lookbackId(Instruction ins) {
        Instruction p = ins.getPrevious();
        for (int i = 0; i < 8 && p != null; i++, p = p.getPrevious()) {
            String m = p.getMnemonicString().toLowerCase();
            if (m.equals("mov") || m.equals("movl")) {
                Object[] ops = p.getOpObjects(1);
                if (ops.length == 1 && ops[0] instanceof Scalar) {
                    return ((Scalar) ops[0]).getUnsignedValue();
                }
            }
        }
        return -1;
    }

    @Override
    public void run() throws Exception {
        Listing lst = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        println("### SYSCALL STUBS");
        Set<Address> writeWrappers = new HashSet<>();
        Set<Address> readWrappers = new HashSet<>();
        for (Function f : fm.getFunctions(true)) {
            InstructionIterator it = lst.getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (!ins.getMnemonicString().equalsIgnoreCase("SYSCALL")) continue;
                long id = lookbackId(ins);
                println(String.format("syscall in %s @ %s id=0x%x", f.getName(),
                        ins.getAddress(), id));
                if (id == 0x3a) writeWrappers.add(f.getEntryPoint());
                if (id == 0x3f) readWrappers.add(f.getEntryPoint());
            }
        }
        println("### WRITE WRAPPERS: " + writeWrappers);

        println("### CALL SITES OF WRITE WRAPPER");
        int calls = 0;
        for (Function f : fm.getFunctions(true)) {
            InstructionIterator it = lst.getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (!ins.getFlowType().isCall()) continue;
                for (Address t : ins.getFlows()) {
                    if (!writeWrappers.contains(t)) continue;
                    calls++;
                    println(String.format("WRITE-CALL caller=%s @ %s", f.getName(),
                            ins.getAddress()));
                    dumpBefore(ins, 16);
                }
            }
        }
        println("### WRITE CALL SITES: " + calls);

        println("### AIM-ISH STRINGS AND REFS");
        for (Data d : lst.getDefinedData(true)) {
            if (d == null || d.getValue() == null) continue;
            String s = d.getValue().toString();
            if (!(s.contains("aim") || s.contains("Aim") || s.contains("trigger") ||
                  s.contains("Trigger"))) continue;
            if (s.length() > 60) continue;
            println(String.format("str @ %s = %s", d.getAddress(), s));
            for (Reference r : getReferencesTo(d.getAddress())) {
                println(String.format("    ref from %s", r.getFromAddress()));
            }
        }
    }
}
