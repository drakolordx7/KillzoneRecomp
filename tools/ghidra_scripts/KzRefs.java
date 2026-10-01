// Lists functions that reference the given data addresses. Args: plus-separated addrs, out file
// @category KillzoneRecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

public class KzRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(a[1]);
        for (String s : a[0].split("[+]")) {
            out.println("# " + s);
            TreeSet<String> fs = new TreeSet<>();
            for (Reference r : getReferencesTo(toAddr(s.trim()))) {
                Function c = getFunctionContaining(r.getFromAddress());
                fs.add((c == null ? "?" : c.getName()) + "  [" + r.getFromAddress() + " " + r.getReferenceType() + "]");
            }
            for (String x : fs) out.println("  " + x);
        }
        out.close();
    }
}
