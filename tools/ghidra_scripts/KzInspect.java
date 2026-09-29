// Decompile functions containing given addresses and list xrefs to given data addresses.
// Args: plus-separated code addrs, plus-separated data addrs, output file
// @category KillzoneRecomp
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

public class KzInspect extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String[] code = (a[0].isEmpty() || a[0].equals("-")) ? new String[0] : a[0].split("[+]");
        String[] data = a.length > 1 && !a[1].isEmpty() && !a[1].equals("-") ? a[1].split("[+]") : new String[0];
        PrintWriter out = new PrintWriter(a[2]);
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Function> done = new LinkedHashSet<>();
        List<Function> todo = new ArrayList<>();
        for (String s : code) {
            Function f = getFunctionContaining(toAddr(s.trim()));
            out.println("# code " + s + " -> " + (f == null ? "NO FUNCTION" : f.getName() + " @ " + f.getEntryPoint()));
            if (f != null) todo.add(f);
        }
        for (String s : data) {
            Address d = toAddr(s.trim());
            out.println("# xrefs to " + s + ":");
            for (Reference r : getReferencesTo(d)) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("   " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (f == null ? "?" : f.getName()));
                if (f != null) todo.add(f);
            }
        }
        for (Function f : todo) {
            if (!done.add(f)) continue;
            DecompileResults res = di.decompileFunction(f, 60, monitor);
            out.println("\n//==== " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
            out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed: " + res.getErrorMessage());
        }
        out.close();
    }
}
