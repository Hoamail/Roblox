// Decompile the function containing a given address (and its callees one level).
// usage: -postScript VelFn.java 0x7ff7ddc83e66 [depth]
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class VelFn extends GhidraScript {
    private DecompInterface dec;

    private String decompile(Function f) {
        DecompileResults r = dec.decompileFunction(f, 60, monitor);
        if (r == null || !r.decompileCompleted())
            return "/* decompile failed: " + (r == null ? "null" : r.getErrorMessage()) + " */";
        return r.getDecompiledFunction().getC();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long addr = Long.decode(args[0]);
        int depth = args.length > 1 ? Integer.decode(args[1]) : 0;

        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();
        Address a = toAddr(addr);
        Function f = fm.getFunctionContaining(a);
        if (f == null) {
            println("no function contains " + a);
            return;
        }
        println("### FUNCTION " + f.getName() + " entry=" + f.getEntryPoint()
                + " size=" + f.getBody().getNumAddresses());
        println(decompile(f));

        println("### CALLERS");
        for (Function caller : f.getCallingFunctions(monitor)) {
            println("### CALLER " + caller.getName() + " entry=" + caller.getEntryPoint()
                    + " size=" + caller.getBody().getNumAddresses());
            if (depth >= 2) println(decompile(caller));
        }
        if (depth > 0) {
            for (Function callee : f.getCalledFunctions(monitor)) {
                if (callee.isThunk() || callee.isExternal()) continue;
                println("### CALLEE " + callee.getName() + " entry=" + callee.getEntryPoint()
                        + " size=" + callee.getBody().getNumAddresses());
                println(decompile(callee));
            }
        }
    }
}
