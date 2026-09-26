// Apply the v1.0 race.exe linker map (out/symbols.csv, from tools/extract_map.py) to a Ghidra program.
// Run it BEFORE auto-analysis (analyzeHeadless -preScript, or from the Script Manager on a fresh
// import) so that Ghidra's Microsoft demangler turns every mangled label into a typed signature --
// class, calling convention, parameters and return type -- as part of the analysis.
//   analyzeHeadless <proj> ViperRacing -import race_v10.exe -scriptPath tools -preScript ApplyViperMap.java out\symbols.csv
//@category ViperPort
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

public class ApplyViperMap extends GhidraScript {

    /** One CSV line, honouring double-quoted fields (demangled names contain commas). */
    private static List<String> split(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (quoted) {
                if (c == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') { cur.append('"'); i++; }
                else if (c == '"') quoted = false;
                else cur.append(c);
            } else if (c == '"') quoted = true;
            else if (c == ',') { out.add(cur.toString()); cur.setLength(0); }
            else cur.append(c);
        }
        out.add(cur.toString());
        return out;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File csv = args.length > 0 ? new File(args[0]) : askFile("symbols.csv from tools/extract_map.py", "Apply");
        int named = 0, made = 0, failed = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(csv))) {
            String header = r.readLine();                       // va,section,kind,name,demangled,library,object
            String line;
            while ((line = r.readLine()) != null) {
                List<String> f = split(line);
                Address a = toAddr(Long.parseLong(f.get(0), 16));
                String section = f.get(1), kind = f.get(2), name = f.get(3), lib = f.get(5), obj = f.get(6);
                try {
                    currentProgram.getSymbolTable().createLabel(a, name, SourceType.IMPORTED);
                    named++;
                } catch (Exception ex) {
                    failed++;
                }
                if (kind.equals("function") && section.equals(".text")) {
                    Function fn = getFunctionAt(a);
                    if (fn == null) {
                        fn = createFunction(a, null);
                        if (fn != null) made++;
                    }
                    if (fn != null) {
                        String src = (lib.equals("root") || lib.equals("crt")) ? obj : lib + ":" + obj;
                        fn.setComment("from " + src);
                    }
                }
            }
        }
        println("ApplyViperMap: named " + named + " addresses, created " + made + " functions, " + failed + " labels refused");
    }
}
