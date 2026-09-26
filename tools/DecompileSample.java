// Decompile a handful of well-understood functions to a C file, as a check on the imported names and
// types. Arguments: an output path, then function names (Ghidra's demangled names, e.g.
// "Obstacle::Update"). Run after analysis:
//   analyzeHeadless <proj> ViperRacing -process race_v10.exe -noanalysis -scriptPath tools
//       -postScript DecompileSample.java out\decomp_sample.c Obstacle::Update ...
//@category ViperPort
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.PrintWriter;

public class DecompileSample extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        int done = 0;
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
                while (it.hasNext()) {
                    Function f = it.next();
                    if (!f.getName(true).equals(args[i])) continue;
                    DecompileResults res = d.decompileFunction(f, 60, monitor);
                    out.println("// ---- " + f.getName(true) + " @ " + f.getEntryPoint() + "  (" + f.getComment() + ")");
                    out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed: " + res.getErrorMessage());
                    done++;
                    break;
                }
            }
        }
        println("DecompileSample: wrote " + done + " functions to " + args[0]);
    }
}
