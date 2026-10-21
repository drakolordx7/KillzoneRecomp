// Lists instructions whose scalar operand equals one of the given offsets. Args: comma-separated offsets (hex), out file, optional mnemonic prefix filter
// @category KillzoneRecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.PrintWriter;
import java.util.*;

public class KzScanOff extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Set<Long> offs = new HashSet<>();
        for (String s : a[0].split(",")) offs.add(Long.decode(s.trim()));
        String mn = a.length > 2 ? a[2] : "";
        PrintWriter out = new PrintWriter(a[1]);
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction in = it.next();
            if (!mn.isEmpty() && !in.getMnemonicString().startsWith(mn)) continue;
            for (int i = 0; i < in.getNumOperands(); i++) {
                boolean hit = false;
                for (Object o : in.getOpObjects(i)) if (o instanceof Scalar && offs.contains(((Scalar) o).getSignedValue())) hit = true;
                if (hit) {
                    Function f = getFunctionContaining(in.getAddress());
                    out.println(in.getAddress() + "  " + (f == null ? "?" : f.getName()) + "  " + in);
                    break;
                }
            }
        }
        out.close();
    }
}
