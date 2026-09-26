// Print named structures from the project's data types: every copy with that name, its path and its
// components. Arguments: an output path, then structure names.
//   analyzeHeadless <proj> ViperRacing -process race_v10.exe -noanalysis -readOnly -scriptPath tools
//       -postScript DumpTypes.java out\types_dump.txt PhobRoot PhobDyno
//@category ViperPort
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class DumpTypes extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                List<DataType> all = new ArrayList<>();
                dtm.findDataTypes(args[i], all);
                for (DataType d : all) {
                    out.println(d.getPathName() + "  (" + d.getClass().getSimpleName() + ", " + d.getLength() + " bytes)");
                    if (!(d instanceof Structure)) continue;
                    for (DataTypeComponent c : ((Structure) d).getDefinedComponents())
                        out.printf("  +0x%-4x %-28s %s%n", c.getOffset(), c.getDataType().getName(),
                                   c.getFieldName() == null ? "" : c.getFieldName());
                }
            }
        }
    }
}
