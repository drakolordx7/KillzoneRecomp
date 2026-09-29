// Lists callers (recursively up to depth) of the given function addresses. Args: plus-separated addrs, depth, out file
// @category KillzoneRecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

public class KzCallers extends GhidraScript {
    PrintWriter out;
    void walk(Function f, int depth, int max, Set<Function> seen) {
        if (depth > max || !seen.add(f)) return;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            out.println("  ".repeat(depth) + "<- " + (c == null ? "?" : c.getName() + " @ " + c.getEntryPoint()) + "  [" + r.getFromAddress() + " " + r.getReferenceType() + "]");
            if (c != null) walk(c, depth + 1, max, seen);
        }
    }
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        out = new PrintWriter(a[2]);
        int max = Integer.parseInt(a[1]);
        for (String s : a[0].split("[+]")) {
            Function f = getFunctionContaining(toAddr(s.trim()));
            out.println("# " + s + " = " + (f == null ? "?" : f.getName()));
            if (f != null) walk(f, 1, max, new HashSet<>());
        }
        out.close();
    }
}
